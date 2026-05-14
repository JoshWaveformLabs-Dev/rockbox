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

#include "config.h"

#ifdef WAVEFORM_TELEMETRY

#include <string.h>
#include "wf_telemetry.h"
#include "system.h"     /* disable_irq_save / restore_irq */
#include "kernel.h"     /* current_tick */
#ifdef WAVEFORM_TELEMETRY_BUFLIB
#include "core_alloc.h" /* core_available / core_allocatable */
#endif

/* Backing ring in BSS — zero-initialised at boot, no allocation. */
static struct wf_event wf_event_ring[WF_EVENT_RING_SIZE];
static volatile size_t wf_event_head;   /* next write index, [0, RING_SIZE) */
static volatile size_t wf_event_total;  /* total records ever written */

void wf_event_record(uint16_t sub, uint16_t evt,
                     uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    int irq = disable_irq_save();
    size_t slot = wf_event_head;
    struct wf_event *e = &wf_event_ring[slot];
    e->tick = (uint32_t)current_tick;
    e->subsystem = sub;
    e->event = evt;
    e->a = a;
    e->b = b;
    e->c = c;
    e->d = d;
    e->reserved = 0;
    wf_event_head = (slot + 1) % WF_EVENT_RING_SIZE;
    wf_event_total++;
    restore_irq(irq);
}

const struct wf_event *wf_event_snapshot(size_t *count_out, size_t *head_out)
{
    if (count_out)
        *count_out = wf_event_total;
    if (head_out)
        *head_out = wf_event_head;
    return wf_event_ring;
}

void wf_event_reset(void)
{
    int irq = disable_irq_save();
    memset(wf_event_ring, 0, sizeof(wf_event_ring));
    wf_event_head = 0;
    wf_event_total = 0;
    restore_irq(irq);
}

/* ----- D2: buflib counters ------------------------------------------- */
#ifdef WAVEFORM_TELEMETRY_BUFLIB

/* last_total_free is maintained arithmetically by note_alloc / note_free
 * (delta-only). It starts at 0 (before any compact_done has fired) and
 * may briefly underflow to 0 if alloc precedes the first ground-truth
 * sample. compact_done and wf_buflib_sample_now() re-sync to the real
 * walked value. The drift is bounded: handle-table growth (one union
 * buflib_data slot per *new* handle) is the only un-modeled mutation —
 * negligible vs typical buflib block sizes. */
static struct wf_buflib_counters wf_buflib_state = {
    .min_largest_contig = UINT32_MAX,
};

/* Internal: update the largest-contig floor when we have a fresh sample.
 * Caller holds IRQ-disabled critical section (or guarantees no concurrent
 * sample). The hot-path alloc/free helpers do NOT call this — only the
 * compact-done hook and the explicit sample-now helper do. */
static void wf_buflib_update_floor_locked(uint32_t total_free, uint32_t largest_contig)
{
    wf_buflib_state.last_total_free     = total_free;
    wf_buflib_state.last_largest_contig = largest_contig;
    if (largest_contig < wf_buflib_state.min_largest_contig)
    {
        wf_buflib_state.min_largest_contig = largest_contig;
        wf_event_record(WF_SUB_BUFLIB, WF_EVT_BUFLIB_FRAG_LOW,
                        largest_contig, total_free, 0, 0);
    }
}

void wf_buflib_note_alloc(size_t size)
{
    uint32_t cached_free;
    int irq = disable_irq_save();
    wf_buflib_state.alloc_count++;
    /* Arithmetic update — saturating subtract guards against stale cache. */
    if (wf_buflib_state.last_total_free >= (uint32_t)size)
        wf_buflib_state.last_total_free -= (uint32_t)size;
    else
        wf_buflib_state.last_total_free = 0;
    cached_free = wf_buflib_state.last_total_free;
    restore_irq(irq);
    wf_event_record(WF_SUB_BUFLIB, WF_EVT_BUFLIB_ALLOC,
                    (uint32_t)size, cached_free, 0, 0);
}

void wf_buflib_note_free(size_t freed)
{
    uint32_t cached_free;
    int irq = disable_irq_save();
    wf_buflib_state.free_count++;
    wf_buflib_state.last_total_free += (uint32_t)freed;
    cached_free = wf_buflib_state.last_total_free;
    restore_irq(irq);
    wf_event_record(WF_SUB_BUFLIB, WF_EVT_BUFLIB_FREE,
                    (uint32_t)freed, cached_free, 0, 0);
}

void wf_buflib_note_move(int handle, const void *from, const void *to)
{
    int irq = disable_irq_save();
    wf_buflib_state.move_count++;
    restore_irq(irq);
    wf_event_record(WF_SUB_BUFLIB, WF_EVT_BUFLIB_MOVE,
                    (uint32_t)handle,
                    (uint32_t)(uintptr_t)from,
                    (uint32_t)(uintptr_t)to, 0);
}

void wf_buflib_note_compact_begin(void)
{
    int irq = disable_irq_save();
    wf_buflib_state.compact_count++;
    restore_irq(irq);
    wf_event_record(WF_SUB_BUFLIB, WF_EVT_BUFLIB_COMPACT, 0, 0, 0, 0);
}

void wf_buflib_note_compact_done(size_t total_free, size_t largest_contig)
{
    int irq = disable_irq_save();
    wf_buflib_update_floor_locked((uint32_t)total_free, (uint32_t)largest_contig);
    restore_irq(irq);
}

void wf_buflib_sample_now(void)
{
    /* Caller is in normal context (e.g. debug menu). core_allocatable() may
     * invoke compaction; that's acceptable because this is user-initiated. */
    size_t total = core_available();
    size_t large = core_allocatable();
    int irq = disable_irq_save();
    wf_buflib_update_floor_locked((uint32_t)total, (uint32_t)large);
    restore_irq(irq);
}

void wf_buflib_note_pin(int handle, unsigned new_pincount)
{
    uint32_t cur;
    int irq = disable_irq_save();
    wf_buflib_state.pin_count++;
    /* Transition 0→1: a previously unpinned handle just became pinned. */
    if (new_pincount == 1)
    {
        wf_buflib_state.cur_pinned_handles++;
        if (wf_buflib_state.cur_pinned_handles > wf_buflib_state.peak_pinned_handles)
            wf_buflib_state.peak_pinned_handles = wf_buflib_state.cur_pinned_handles;
    }
    cur = wf_buflib_state.cur_pinned_handles;
    restore_irq(irq);
    wf_event_record(WF_SUB_BUFLIB, WF_EVT_BUFLIB_PIN, (uint32_t)handle,
                    cur, (uint32_t)new_pincount, 0);
}

void wf_buflib_note_unpin(int handle, unsigned new_pincount)
{
    uint32_t cur;
    int irq = disable_irq_save();
    wf_buflib_state.unpin_count++;
    /* Transition 1→0: this handle no longer has any outstanding pins. */
    if (new_pincount == 0 && wf_buflib_state.cur_pinned_handles > 0)
        wf_buflib_state.cur_pinned_handles--;
    cur = wf_buflib_state.cur_pinned_handles;
    restore_irq(irq);
    wf_event_record(WF_SUB_BUFLIB, WF_EVT_BUFLIB_UNPIN, (uint32_t)handle,
                    cur, (uint32_t)new_pincount, 0);
}

void wf_buflib_counters_get(struct wf_buflib_counters *out)
{
    int irq = disable_irq_save();
    *out = wf_buflib_state;
    restore_irq(irq);
}

uint32_t wf_buflib_frag_ratio_x10000(void)
{
    uint32_t total  = wf_buflib_state.last_total_free;
    uint32_t largest = wf_buflib_state.last_largest_contig;
    if (total == 0)
        return 0;
    /* clamp 0..10000 in case of stale samples */
    uint64_t r = (uint64_t)largest * 10000u / total;
    return r > 10000 ? 10000 : (uint32_t)r;
}

#endif /* WAVEFORM_TELEMETRY_BUFLIB */

/* ----- D4: pcmbuf counters ------------------------------------------- */
#ifdef WAVEFORM_TELEMETRY_PCMBUF

/* min_fill_ever needs UINT32_MAX as the "no sample yet" sentinel; the rest
 * of the struct zero-inits cleanly in BSS. */
static struct wf_pcmbuf_counters wf_pcmbuf_state = {
    .min_fill_ever = UINT32_MAX,
};

/* Tick of the most recent low_state entry — used to accumulate the
 * low-state interval on exit (and on note_stopped). */
static uint32_t wf_pcmbuf_low_enter_tick;

void wf_pcmbuf_note_request(uint32_t realrem, uint32_t watermark)
{
    uint32_t now = (uint32_t)current_tick;
    int now_low = (realrem < watermark) ? 1 : 0;
    int was_low;
    uint32_t held = 0;
    /* Emit inside the critical section: LOW_ENTER/LOW_EXIT pairs must stay
     * strictly ordered in the ring relative to the counter state they
     * describe. wf_event_record's own disable_irq_save() nests harmlessly
     * (Rockbox saves/restores prior state, not raw enable). */
    int irq = disable_irq_save();
    if (realrem < wf_pcmbuf_state.min_fill_ever)
        wf_pcmbuf_state.min_fill_ever = realrem;
    was_low = (int)wf_pcmbuf_state.in_low_state;
    if (now_low && !was_low)
    {
        wf_pcmbuf_state.in_low_state = 1;
        wf_pcmbuf_state.low_entry_count++;
        wf_pcmbuf_low_enter_tick = now;
        wf_event_record(WF_SUB_PCMBUF, WF_EVT_PCMBUF_LOW_ENTER,
                        realrem, watermark, now, 0);
    }
    else if (!now_low && was_low)
    {
        held = now - wf_pcmbuf_low_enter_tick;
        wf_pcmbuf_state.low_total_ticks += held;
        wf_pcmbuf_state.low_exit_count++;
        wf_pcmbuf_state.in_low_state = 0;
        wf_event_record(WF_SUB_PCMBUF, WF_EVT_PCMBUF_LOW_EXIT,
                        realrem, watermark, held, 0);
    }
    restore_irq(irq);
}

void wf_pcmbuf_note_stopped(void)
{
    uint32_t now = (uint32_t)current_tick;
    int irq = disable_irq_save();
    if (wf_pcmbuf_state.in_low_state)
    {
        uint32_t held = now - wf_pcmbuf_low_enter_tick;
        wf_pcmbuf_state.low_total_ticks += held;
        wf_pcmbuf_state.low_exit_count++;
        wf_pcmbuf_state.in_low_state = 0;
        wf_event_record(WF_SUB_PCMBUF, WF_EVT_PCMBUF_LOW_EXIT,
                        0, 0, held, 1 /*stopped*/);
    }
    restore_irq(irq);
}

void wf_pcmbuf_counters_get(struct wf_pcmbuf_counters *out)
{
    int irq = disable_irq_save();
    *out = wf_pcmbuf_state;
    restore_irq(irq);
}

#endif /* WAVEFORM_TELEMETRY_PCMBUF */

/* ----- D4: codec counters -------------------------------------------- */
#ifdef WAVEFORM_TELEMETRY_CODEC

static struct wf_codec_record wf_codec_records[WF_CODEC_MAX_FORMATS];
/* Single codec is active at a time (codec_thread.c serialises via
 * codec_type), so one pending pair is enough. AFMT_UNKNOWN==0 doubles as
 * the "no pending" sentinel for the pending_*_afmt fields. */
static uint32_t wf_codec_pending_load_tick;
static uint32_t wf_codec_pending_run_tick;
static uint16_t wf_codec_pending_load_afmt;
static uint16_t wf_codec_pending_run_afmt;

/* Linear scan + first-free allocation. Returns NULL if all slots taken
 * (silent drop — production-stripped set is 5 codecs vs 8 slots). */
static struct wf_codec_record *wf_codec_slot_locked(uint16_t afmt)
{
    struct wf_codec_record *free_slot = NULL;
    for (size_t i = 0; i < WF_CODEC_MAX_FORMATS; i++)
    {
        if (wf_codec_records[i].in_use && wf_codec_records[i].afmt == afmt)
            return &wf_codec_records[i];
        if (!wf_codec_records[i].in_use && free_slot == NULL)
            free_slot = &wf_codec_records[i];
    }
    if (free_slot)
    {
        free_slot->afmt   = afmt;
        free_slot->in_use = 1;
    }
    return free_slot;
}

void wf_codec_note_load_begin(uint16_t afmt)
{
    int irq = disable_irq_save();
    wf_codec_pending_load_tick = (uint32_t)current_tick;
    wf_codec_pending_load_afmt = afmt;
    restore_irq(irq);
    wf_event_record(WF_SUB_CODEC, WF_EVT_CODEC_LOAD_BEGIN, afmt, 0, 0, 0);
}

void wf_codec_note_load_end(uint16_t afmt, int32_t status)
{
    uint32_t now = (uint32_t)current_tick;
    uint32_t elapsed = 0;
    int irq = disable_irq_save();
    if (wf_codec_pending_load_afmt == afmt)
        elapsed = now - wf_codec_pending_load_tick;
    struct wf_codec_record *r = wf_codec_slot_locked(afmt);
    if (r)
    {
        r->load_count++;
        r->load_ticks_last = elapsed;
        if (elapsed > r->load_ticks_max)
            r->load_ticks_max = elapsed;
    }
    wf_codec_pending_load_afmt = 0;
    restore_irq(irq);
    wf_event_record(WF_SUB_CODEC, WF_EVT_CODEC_LOAD_END,
                    afmt, elapsed, (uint32_t)status, 0);
}

void wf_codec_note_run_begin(uint16_t afmt)
{
    int irq = disable_irq_save();
    wf_codec_pending_run_tick = (uint32_t)current_tick;
    wf_codec_pending_run_afmt = afmt;
    restore_irq(irq);
    wf_event_record(WF_SUB_CODEC, WF_EVT_CODEC_RUN_BEGIN, afmt, 0, 0, 0);
}

void wf_codec_note_run_end(uint16_t afmt, int32_t status)
{
    uint32_t now = (uint32_t)current_tick;
    uint32_t elapsed = 0;
    int irq = disable_irq_save();
    if (wf_codec_pending_run_afmt == afmt)
        elapsed = now - wf_codec_pending_run_tick;
    struct wf_codec_record *r = wf_codec_slot_locked(afmt);
    if (r)
    {
        r->run_count++;
        r->run_ticks_last = elapsed;
        if (elapsed > r->run_ticks_max)
            r->run_ticks_max = elapsed;
    }
    wf_codec_pending_run_afmt = 0;
    restore_irq(irq);
    wf_event_record(WF_SUB_CODEC, WF_EVT_CODEC_RUN_END,
                    afmt, elapsed, (uint32_t)status, 0);
}

void wf_codec_get_records(struct wf_codec_record *out, size_t max_records,
                          size_t *count_out)
{
    size_t copied = 0;
    int irq = disable_irq_save();
    for (size_t i = 0; i < WF_CODEC_MAX_FORMATS && copied < max_records; i++)
    {
        if (wf_codec_records[i].in_use)
            out[copied++] = wf_codec_records[i];
    }
    restore_irq(irq);
    if (count_out)
        *count_out = copied;
}

#endif /* WAVEFORM_TELEMETRY_CODEC */

#endif /* WAVEFORM_TELEMETRY */
