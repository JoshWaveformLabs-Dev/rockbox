/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (C) 2026 Waveform Labs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* Waveform OS Stage 1 instrumentation core: binary event ring + per-subsystem
 * counter accessors. Gated by WAVEFORM_TELEMETRY at compile time; production
 * builds leave the flag undefined and the symbols vanish entirely. */

#ifndef _WF_TELEMETRY_H_
#define _WF_TELEMETRY_H_

#include "config.h"

#ifdef WAVEFORM_TELEMETRY

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Subsystem identifiers (uint16_t event.subsystem field). */
enum {
    WF_SUB_NONE     = 0,
    WF_SUB_BUFLIB   = 1,
    WF_SUB_PCMBUF   = 2,
    WF_SUB_CODEC    = 3,
    WF_SUB_STORAGE  = 4,
    WF_SUB_STACK    = 5,
    WF_SUB__COUNT
};

/* Event identifiers (uint16_t event.event field). High byte = subsystem,
 * low byte = event within that subsystem. Reserve room for future events. */
enum {
    WF_EVT_BUFLIB_ALLOC     = 0x0100,
    WF_EVT_BUFLIB_FREE      = 0x0101,
    WF_EVT_BUFLIB_MOVE      = 0x0102,
    WF_EVT_BUFLIB_COMPACT   = 0x0103,
    WF_EVT_BUFLIB_PIN       = 0x0104,
    WF_EVT_BUFLIB_UNPIN     = 0x0105,
    WF_EVT_BUFLIB_FRAG_LOW  = 0x0106, /* largest-contig dipped below prior min */

    WF_EVT_PCMBUF_LOW_ENTER = 0x0200,
    WF_EVT_PCMBUF_LOW_EXIT  = 0x0201,

    WF_EVT_CODEC_LOAD_BEGIN = 0x0300,
    WF_EVT_CODEC_LOAD_END   = 0x0301,
    WF_EVT_CODEC_RUN_BEGIN  = 0x0302,
    WF_EVT_CODEC_RUN_END    = 0x0303,

    WF_EVT_STORAGE_READ_BEGIN  = 0x0400,
    WF_EVT_STORAGE_READ_END    = 0x0401,
    WF_EVT_STORAGE_WRITE_BEGIN = 0x0402,
    WF_EVT_STORAGE_WRITE_END   = 0x0403,
    WF_EVT_STORAGE_WAKE        = 0x0404,

    WF_EVT_STACK_OVERRUN = 0x0500,
    WF_EVT_STACK_LOW     = 0x0501  /* min-remaining dropped below prior floor */
};

/* 32-byte event record. The fields are deliberately untyped past (sub,evt) —
 * subsystem decides what a/b/c/d mean. Decode is offline (scripts/wf-trace-decode.py). */
struct wf_event {
    uint32_t tick;       /* current_tick at record time */
    uint16_t subsystem;  /* WF_SUB_* */
    uint16_t event;      /* WF_EVT_* */
    uint32_t a, b, c, d; /* subsystem-defined payload */
    uint32_t reserved;   /* pad to 32 bytes; ignored on decode */
};

#ifndef WF_EVENT_RING_SIZE
/* 1024 entries * 32 bytes = 32 KB BSS. Drop to 256 on memory-constrained
 * targets — see plan deliverable D1 risks. */
#define WF_EVENT_RING_SIZE 1024
#endif

/* Record one event. Safe from any context including ISR (uses IRQ-save). */
void wf_event_record(uint16_t sub, uint16_t evt,
                     uint32_t a, uint32_t b, uint32_t c, uint32_t d);

/* Snapshot the ring. Returns pointer to the backing array of WF_EVENT_RING_SIZE
 * slots. *count_out is the total event count (may exceed ring size — wrap
 * happens at WF_EVENT_RING_SIZE). *head_out is the next-write index (oldest
 * valid record is at head if count >= ring size, otherwise at 0). */
const struct wf_event *wf_event_snapshot(size_t *count_out, size_t *head_out);

/* Reset ring (clears all records and counter). Used for fresh per-scene capture. */
void wf_event_reset(void);

/* ----- D2: buflib subsystem ------------------------------------------- */
#ifdef WAVEFORM_TELEMETRY_BUFLIB

/* Counter snapshot exposed to debug menu / dump scripts.
 *
 * Pin tracking scope: cur/peak_pinned_handles count distinct handles that
 * have an outstanding explicit buflib_pin() (i.e. 0→1 / 1→0 transitions).
 * The inline buflib_get_data_pinned() / buflib_put_data_pinned() pair in
 * include/buflib_mempool.h bypasses telemetry by design — those are
 * per-access scoped and would dwarf the signal. pin_count/unpin_count are
 * total explicit-call counts (every pin/unpin, including refcount bumps). */
struct wf_buflib_counters {
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t move_count;
    uint32_t compact_count;
    uint32_t pin_count;
    uint32_t unpin_count;
    uint32_t cur_pinned_handles;   /* distinct handles currently pinned */
    uint32_t peak_pinned_handles;  /* peak of cur_pinned_handles */
    uint32_t min_largest_contig;   /* lifetime floor of buflib_allocatable() */
    uint32_t last_total_free;
    uint32_t last_largest_contig;
};

void wf_buflib_counters_get(struct wf_buflib_counters *out);

/* largest-contiguous / total-free, scaled 0..10000. 10000 = perfectly defragged.
 * Returns 0 when total_free is 0. This is the "gold metric" per
 * docs/research/INSTRUMENTATION_STRATEGY.md. */
uint32_t wf_buflib_frag_ratio_x10000(void);

/* Hot-path hooks. Called from firmware/buflib_mempool.c — keep cheap.
 *
 * last_total_free is maintained arithmetically (delta-only): note_alloc
 * subtracts size from the cached value, note_free adds freed bytes. This
 * avoids walking the heap on every alloc/free. Full re-sync of total_free
 * and largest_contig happens only in note_compact_done (buffer is naturally
 * contiguous so the walks are effectively free) and in wf_buflib_sample_now
 * (debug menu, user-initiated). */
void wf_buflib_note_alloc(size_t size);
void wf_buflib_note_free(size_t freed);
void wf_buflib_note_move(int handle, const void *from, const void *to);
void wf_buflib_note_compact_begin(void);
void wf_buflib_note_compact_done(size_t total_free, size_t largest_contig);
/* Pin hooks: pass the NEW pincount value (after increment / before pre-decrement
 * reflects the to-be-stored value). cur_pinned_handles only changes on
 * 0→1 (note_pin with new_pincount==1) and 1→0 (note_unpin with new_pincount==0). */
void wf_buflib_note_pin(int handle, unsigned new_pincount);
void wf_buflib_note_unpin(int handle, unsigned new_pincount);

/* Explicit sample — for debug menu. Walks the heap, refreshes last_* fields
 * and updates the min-largest-contig floor. Safe to call from idle context. */
void wf_buflib_sample_now(void);

#endif /* WAVEFORM_TELEMETRY_BUFLIB */

/* ----- D3: stack canary scanner --------------------------------------- */
#ifdef WAVEFORM_TELEMETRY_STACK

#define WF_STACK_NAME_MAX 32

/* Per-thread record. Populated by wf_stack_scan_now() / wf_stack_get_records(). */
struct wf_stack_record {
    char     name[WF_STACK_NAME_MAX];
    uint32_t thread_id;
    uint32_t stack_size;       /* bytes */
    uint32_t peak_used;        /* lifetime peak via DEADBEEF scan, bytes */
    uint32_t last_used;        /* most recent scan reading, bytes */
    uint8_t  in_use;           /* slot has a live thread */
    uint8_t  overrun;          /* canary at stack[0] no longer DEADBEEF */
    uint8_t  pad[2];
};

/* Scan all thread slots. Walks each thread's DEADBEEF region, updates the
 * per-slot peak_used / last_used / overrun. Records WF_EVT_STACK_LOW on a
 * fresh peak and WF_EVT_STACK_OVERRUN on canary breach.
 *
 * Stage 1 limitation: on-demand only (called from debug menu or codec
 * lifecycle hooks). Not driven from the tick callback because the scan
 * needs corelocks that cannot be taken from IRQ context. A periodic
 * worker thread is a Stage 2 follow-up. */
void wf_stack_scan_now(void);

/* Copy records into caller-provided array. out_records must hold at least
 * max_records entries. count_out (if non-NULL) returns the populated count
 * (capped at min(max_records, MAXTHREADS)). */
void wf_stack_get_records(struct wf_stack_record *out_records,
                          size_t max_records, size_t *count_out);

uint32_t wf_stack_get_overrun_total(void);
uint32_t wf_stack_get_scan_count(void);

#endif /* WAVEFORM_TELEMETRY_STACK */

/* ----- D4: pcmbuf subsystem ------------------------------------------- */
#ifdef WAVEFORM_TELEMETRY_PCMBUF

/* Edge-triggered watermark-cross telemetry. low_entry_count counts 0->1
 * transitions of (realrem < watermark); low_exit_count counts 1->0. The
 * state machine is internal — callers just feed every realrem/watermark
 * observation in via wf_pcmbuf_note_request() and we decide whether the
 * sample crosses an edge. min_fill_ever is the smallest realrem ever
 * observed (initialised to UINT32_MAX on first call). low_total_ticks
 * accumulates ticks held in the below-watermark state across runs. */
struct wf_pcmbuf_counters {
    uint32_t low_entry_count;
    uint32_t low_exit_count;
    uint32_t min_fill_ever;       /* bytes; UINT32_MAX = no sample yet */
    uint32_t low_total_ticks;
    uint32_t in_low_state;        /* 0 / 1 — current edge state */
};

/* Hot-path hook. Call from pcmbuf_request_buffer() — realrem = bytes used
 * in pcm buffer (pcmbuf_size - freespace), watermark = pcmbuf_watermark. */
void wf_pcmbuf_note_request(uint32_t realrem, uint32_t watermark);

/* Called from the channel-stopped branch — closes out any open low-state
 * interval so we don't accumulate ticks while playback is halted. */
void wf_pcmbuf_note_stopped(void);

void wf_pcmbuf_counters_get(struct wf_pcmbuf_counters *out);

#endif /* WAVEFORM_TELEMETRY_PCMBUF */

/* ----- D4: codec subsystem -------------------------------------------- */
#ifdef WAVEFORM_TELEMETRY_CODEC

/* 8 slots vs the post-strip 5-codec set (MP3/AAC/FLAC/ALAC/Opus + headroom
 * for AAC_BSF and any future addition). Slot exhaustion is silent — the
 * stripped target will never reach it. */
#define WF_CODEC_MAX_FORMATS 8

struct wf_codec_record {
    uint16_t afmt;                /* AFMT_* from lib/rbcodec/metadata/metadata.h */
    uint16_t in_use;              /* 0 = empty slot, 1 = populated */
    uint32_t load_count;
    uint32_t load_ticks_last;
    uint32_t load_ticks_max;
    uint32_t run_count;
    uint32_t run_ticks_last;      /* codec-lifetime ticks, not per-frame */
    uint32_t run_ticks_max;
};

/* Lifecycle hooks. A single codec is active at any time (codec_thread.c
 * guards with codec_type != AFMT_UNKNOWN), so internal pending-tick state
 * is a single pair rather than per-afmt. note_load_end / note_run_end
 * compute elapsed against the stored pending tick and update the slot's
 * last + max + count. Each hook also records a WF_EVT_CODEC_* event into
 * the binary ring for offline analysis. */
void wf_codec_note_load_begin(uint16_t afmt);
void wf_codec_note_load_end(uint16_t afmt, int32_t status);
void wf_codec_note_run_begin(uint16_t afmt);
void wf_codec_note_run_end(uint16_t afmt, int32_t status);

/* Copy the populated slots into caller-provided array. *count_out (if
 * non-NULL) returns how many slots are in_use, capped at max_records. */
void wf_codec_get_records(struct wf_codec_record *out, size_t max_records,
                          size_t *count_out);

#endif /* WAVEFORM_TELEMETRY_CODEC */

/* ----- D5: storage subsystem ------------------------------------------ */
#ifdef WAVEFORM_TELEMETRY_STORAGE

/* 16 log2(ticks) buckets: 0, 1, 2, 4, 8, ..., 16384, >=32768. HZ=100 on
 * iPod target → bucket 7 = 128 ticks ≈ 1.28 s. The rockpod observed
 * ~530 ms HDD wake (docs/research/ROCKPOD_REFERENCE.md), so the histogram
 * resolves both fast-flash (≤1 tick) and slow-HDD-wake (≥50 tick) cleanly. */
#define WF_STORAGE_HIST_BUCKETS 16

/* Per-op counters. Reads and writes are split because seek pattern and
 * wake-cost diverge (writes are typically batched flushes; reads dominate
 * playback). Sequential detection: last_*_end_lba is "first LBA AFTER
 * prior range", so sequential iff next start == last_end. */
struct wf_storage_counters {
    uint32_t read_count;
    uint32_t write_count;
    uint32_t read_hist[WF_STORAGE_HIST_BUCKETS];
    uint32_t write_hist[WF_STORAGE_HIST_BUCKETS];
    uint32_t max_read_ticks;
    uint32_t max_write_ticks;
    uint32_t sequential_reads;
    uint32_t discontinuous_reads;
    uint32_t sequential_writes;
    uint32_t discontinuous_writes;
    uint32_t wake_count;          /* requests >100 ticks (1 s) after prior end */
    uint32_t max_wake_gap_ticks;
};

/* Hot-path hooks. Call sites pass drive via IF_MD_DRV(drive) (mv.h) so
 * single-drive builds synthesise 0 and multi-drive builds forward the
 * real index — keeps this header free of mv.h. start is sector_t
 * (uint64_t); event payload stores the low 32 bits (iPod 6g/7g sectors
 * fit comfortably; decode script documents the truncation).
 *
 * Ordering: events emit INSIDE the IRQ-save critical section (matches
 * D4 pcmbuf — see DECISIONS.md 2026-05-14). BEGIN/END/WAKE ordering in
 * the ring must strictly track counter state. */
void wf_storage_note_read_begin(int drive, uint64_t start, int count);
void wf_storage_note_read_end(int drive, uint64_t start, int count, int rc);
void wf_storage_note_write_begin(int drive, uint64_t start, int count);
void wf_storage_note_write_end(int drive, uint64_t start, int count, int rc);

void wf_storage_counters_get(struct wf_storage_counters *out);

#endif /* WAVEFORM_TELEMETRY_STORAGE */

#endif /* WAVEFORM_TELEMETRY */
#endif /* _WF_TELEMETRY_H_ */
