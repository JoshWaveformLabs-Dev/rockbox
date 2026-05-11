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

#endif /* WAVEFORM_TELEMETRY */
#endif /* _WF_TELEMETRY_H_ */
