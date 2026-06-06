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

/* Waveform OS Stage 6.3 D4: in-firmware codec benchmark.
 *
 * Decodes a fixed corpus of single-codec files (one per supported codec)
 * with PCM output discarded, measures wall-clock decode time against the
 * track's reported duration, and reports per-codec realtime ratios. Wired
 * into the WF: debug menu via dbg_wf_codec_bench() in apps/debug_menu.c.
 *
 * Why an in-firmware bench (not a plugin)?  Production WaveformOS strips
 * HAVE_ROCKBOX_PLUGINS, so the upstream test_codec plugin isn't reachable.
 * The bench engine here calls firmware-exported codec_load_file() /
 * codec_run_proc() / codec_close() directly and supplies its own struct
 * codec_api so the same production binary can self-benchmark.
 *
 * Corpus layout (user-provided, copied once during install):
 *   /.rockbox/wf_bench/mpa.mp3     (MP3 / MPEG layer 3)
 *   /.rockbox/wf_bench/flac.flac   (FLAC)
 *   /.rockbox/wf_bench/alac.m4a    (Apple Lossless in MP4)
 *   /.rockbox/wf_bench/aac.m4a     (AAC in MP4)
 *   /.rockbox/wf_bench/opus.opus   (Opus)
 *
 * Missing files are sentinel-recorded as WF_BENCH_SKIP_NO_FILE; the sweep
 * never aborts because of a single missing or broken corpus entry.
 *
 * Corpus files should be 30-60 seconds each. Longer than 60 s is wasteful
 * (the realtime ratio is established within the first few seconds of decode)
 * and any track whose id3.length exceeds WF_BENCH_MAX_DURATION_MS is
 * rejected at the metadata stage with WF_BENCH_SKIP_TOO_LONG; this prevents
 * a multi-minute decode from being mistaken for a freeze by the operator.
 *
 * Audio MUST be stopped before calling wf_codec_bench_run_one(); the caller
 * (debug screen) is responsible for the audio_status() check. The bench
 * commandeers the codec thread via codec_thread_do_callback(), so a running
 * playback codec would conflict. */

#ifndef _WF_CODEC_BENCH_H_
#define _WF_CODEC_BENCH_H_

#include "config.h"

#ifdef WAVEFORM_TELEMETRY_BENCH

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum wf_bench_status {
    WF_BENCH_OK              = 0,
    WF_BENCH_SKIP_NO_FILE    = 1,  /* corpus file missing or unreadable */
    WF_BENCH_ERR_METADATA    = 2,  /* get_metadata() returned false */
    WF_BENCH_ERR_LOAD        = 3,  /* codec_load_file() < 0 */
    WF_BENCH_ERR_RUN         = 4,  /* codec_run_proc() returned CODEC_ERROR */
    WF_BENCH_ABORTED         = 5,  /* user cancelled mid-run */
    WF_BENCH_SKIP_TOO_LONG   = 6,  /* id3.length > WF_BENCH_MAX_DURATION_MS */
};

/* Hard cap on per-track duration (90 s = 60 s recommendation + slack).
 * Tracks longer than this are rejected with WF_BENCH_SKIP_TOO_LONG before
 * codec_load_file() is called, so the operator never has to wait minutes
 * for a long file to decode and mistake the wait for a freeze. */
#define WF_BENCH_MAX_DURATION_MS 90000u

struct wf_bench_result {
    uint16_t              afmt;             /* AFMT_MPA_L3, AFMT_FLAC, ... */
    enum wf_bench_status  status;
    uint32_t              filesize_bytes;
    uint32_t              duration_ms;      /* id3.length */
    uint32_t              decode_ticks;     /* HZ-tick delta, rebuffer subtracted */
    uint32_t              realtime_pct_x100;/* e.g. 23456 = 234.56% realtime */
    uint32_t              mhz_x100;         /* MHz needed for realtime, *100 (0 on hosted) */
};

#define WF_BENCH_CORPUS_COUNT 5

/* Display label for slot i (i in [0, WF_BENCH_CORPUS_COUNT)). Returns a
 * static string ("MP3", "FLAC", ...). */
const char *wf_codec_bench_label(int slot);

/* Build the canonical corpus path for slot i. Returns the path string
 * (points into a static internal buffer; do not free). Returns NULL if
 * slot is out of range. */
const char *wf_codec_bench_path(int slot);

/* Run one codec, blocking until the codec thread finishes or *cancel
 * becomes true. Caller MUST have audio_status() == 0. Result is written
 * to *out regardless of status. Returns 0 on OK / sentinel, -1 on
 * unexpected fatal error. */
int wf_codec_bench_run_one(int slot,
                           struct wf_bench_result *out,
                           volatile bool *cancel);

/* Serialise n results to /.rockbox/wf_codec_bench.txt. Overwrites any
 * existing file. Returns 0 on success, -1 on open/write failure. */
int wf_codec_bench_write_report(const struct wf_bench_result *res, size_t n);

#endif /* WAVEFORM_TELEMETRY_BENCH */

#endif /* _WF_CODEC_BENCH_H_ */
