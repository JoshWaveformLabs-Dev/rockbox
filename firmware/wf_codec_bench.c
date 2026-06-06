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

/* Stage 6.3 D4: in-firmware codec benchmark engine. Public API in
 * firmware/include/wf_codec_bench.h. Structurally mirrors apps/plugins/
 * test_codec.c but supplies its own struct codec_api (no plugin shims),
 * runs from the debug menu thread, and dispatches the actual decode onto
 * the codec thread via codec_thread_do_callback(). PCM is discarded. */

#include "config.h"

#ifdef WAVEFORM_TELEMETRY_BENCH

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>             /* snprintf */
#include <stdlib.h>
#include <string.h>

#include "wf_codec_bench.h"

#include "system.h"            /* cpu_frequency, MIN */
#include "kernel.h"            /* current_tick, HZ, sleep, yield */
#include "file.h"              /* open / close / read / lseek / creat / write / fdprintf */
#include "rbpaths.h"           /* ROCKBOX_DIR */
#include "codecs.h"            /* codec_api, codec_load_file, codec_run_proc, codec_close */
#include "codec_thread.h"      /* codec_thread_do_callback */
#include "metadata.h"          /* get_metadata, audio_formats[], AFMT_* */
#include "dsp_core.h"          /* dsp_get_config, dsp_configure, CODEC_IDX_AUDIO,
                                  DSP_RESET, DSP_FLUSH */
#include "powermgmt.h"         /* reset_poweroff_timer */

/* ------------------------------------------------------------------ *
 *  Corpus table — single source of truth for label / paths / codec.  *
 * ------------------------------------------------------------------ */

struct bench_corpus_entry {
    uint16_t    afmt;
    const char *codec_root_fn;    /* matches audio_formats[afmt].codec_root_fn */
    const char *ext;              /* file extension WITHOUT leading dot */
    const char *display;          /* short label for the screen + report */
    const char *base;             /* basename used in /.rockbox/wf_bench/<base>.<ext> */
};

static const struct bench_corpus_entry bench_corpus[WF_BENCH_CORPUS_COUNT] = {
    { AFMT_MPA_L3,    "mpa",  "mp3",  "MP3",  "mpa"  },
    { AFMT_FLAC,      "flac", "flac", "FLAC", "flac" },
    { AFMT_MP4_ALAC,  "alac", "m4a",  "ALAC", "alac" },
    { AFMT_MP4_AAC,   "aac",  "m4a",  "AAC",  "aac"  },
    { AFMT_OPUS,      "opus", "opus", "Opus", "opus" },
};

/* Path buffer reused across calls — single-thread access from menu only. */
static char bench_path_buf[MAX_PATH];

/* ------------------------------------------------------------------ *
 *  Bench-private codec_api instance + cross-thread state.            *
 * ------------------------------------------------------------------ */

/* The bench's own ci — distinct from the global ci in apps/codecs.c so
 * we never disturb the production playback state machine. */
static struct codec_api bench_ci;

/* Track-scope state (set up before each codec_thread_do_callback). */
static struct mp3entry  bench_id3;
static int              bench_fd = -1;
static off_t            bench_filesize;
static off_t            bench_curr_offset;   /* file offset of bench_buffer[0] */
static size_t           bench_curr_bufsize;  /* live bytes in bench_buffer */
static int              bench_run_rc;        /* codec_run_proc() return, codec-thread side */

/* Sliding-window file buffer. Sized to balance BSS cost (~256 KiB) against
 * rebuffer frequency. Rebuffer ticks are subtracted from decode_ticks so
 * accuracy is preserved regardless of how often we refill — buffer size
 * only affects total wall-clock (i.e. how long the bench takes). */
#define WF_BENCH_BUFSIZE   (256 * 1024)
static unsigned char    bench_buffer[WF_BENCH_BUFSIZE];

/* Cross-thread volatiles. Menu thread writes bench_codec_action; codec
 * thread writes bench_codec_playing / bench_endtick / bench_rebuffer_ticks
 * / bench_run_rc. volatile suffices on PP5022 (single core, cooperative
 * threading); the polling loop's HZ/4 cadence is orders of magnitude
 * larger than any cache-coherence window. See WORKFLOW_RULES.md item 4. */
static volatile bool    bench_codec_playing;
static volatile long    bench_codec_action;
static volatile long    bench_endtick;
static volatile long    bench_rebuffer_ticks;

/* ------------------------------------------------------------------ *
 *  Public helpers.                                                   *
 * ------------------------------------------------------------------ */

const char *wf_codec_bench_label(int slot)
{
    if (slot < 0 || slot >= WF_BENCH_CORPUS_COUNT)
        return "?";
    return bench_corpus[slot].display;
}

const char *wf_codec_bench_path(int slot)
{
    if (slot < 0 || slot >= WF_BENCH_CORPUS_COUNT)
        return NULL;
    snprintf(bench_path_buf, sizeof(bench_path_buf),
             ROCKBOX_DIR "/wf_bench/%s.%s",
             bench_corpus[slot].base,
             bench_corpus[slot].ext);
    return bench_path_buf;
}

/* ------------------------------------------------------------------ *
 *  File-buffer plumbing.                                             *
 * ------------------------------------------------------------------ */

/* Refill bench_buffer starting at file offset new_off. Rebuffer ticks
 * accumulate into bench_rebuffer_ticks so they can be subtracted from
 * the final decode_ticks. Mirrors apps/plugins/test_codec.c fill_buffer. */
static int bench_fill_buffer(off_t new_off)
{
    long temp = current_tick;
    size_t want, got;

    if (lseek(bench_fd, new_off, SEEK_SET) < 0)
        return -1;

    if (new_off + (off_t)WF_BENCH_BUFSIZE <= bench_filesize)
        want = WF_BENCH_BUFSIZE;
    else
        want = bench_filesize - new_off;

    got = read(bench_fd, bench_buffer, want);
    if (got != want)
        return -1;

    bench_curr_offset = new_off;
    bench_curr_bufsize = want;
    bench_rebuffer_ticks += current_tick - temp;
    return 0;
}

/* ------------------------------------------------------------------ *
 *  ci.* callback stubs.                                              *
 * ------------------------------------------------------------------ */

/* Null PCM sink — the whole point of the bench. Pull the poweroff timer
 * so long decodes don't trigger idle suspend. */
static void bench_cb_pcmbuf_insert(const void *ch1, const void *ch2, int count)
{
    (void)ch1; (void)ch2; (void)count;
    reset_poweroff_timer();
}

static void bench_cb_set_elapsed(unsigned long value)
{
    (void)value;   /* bench doesn't render elapsed; codec UI not running */
}

static size_t bench_cb_read_filebuf(void *ptr, size_t size)
{
    size_t realsize;

    if (bench_ci.curpos > bench_filesize)
        return 0;

    realsize = MIN((size_t)(bench_filesize - bench_ci.curpos), size);

    if (realsize > bench_curr_bufsize -
                   (size_t)(bench_ci.curpos - bench_curr_offset))
    {
        if (bench_fill_buffer(bench_ci.curpos) < 0)
            return 0;
    }

    memcpy(ptr,
           bench_buffer + (bench_ci.curpos - bench_curr_offset),
           realsize);
    bench_ci.curpos += realsize;
    return realsize;
}

static void *bench_cb_request_buffer(size_t *realsize, size_t reqsize)
{
    *realsize = MIN((size_t)(bench_filesize - bench_ci.curpos), reqsize);

    if (*realsize > bench_curr_bufsize -
                    (size_t)(bench_ci.curpos - bench_curr_offset))
    {
        if (bench_fill_buffer(bench_ci.curpos) < 0)
        {
            *realsize = 0;
            return NULL;
        }
    }
    return bench_buffer + (bench_ci.curpos - bench_curr_offset);
}

static void bench_cb_advance_buffer(size_t amount)
{
    bench_ci.curpos += amount;
    if (bench_ci.id3)
        bench_ci.id3->offset = bench_ci.curpos;
}

static bool bench_cb_seek_buffer(size_t newpos)
{
    bench_ci.curpos = newpos;
    return true;
}

static void bench_cb_seek_complete(void)
{
}

static void bench_cb_set_offset(size_t value)
{
    if (bench_ci.id3)
        bench_ci.id3->offset = value;
}

/* Forward DSP configuration to the real DSP instance — some codecs read
 * back DSP state for sample-rate upsampling. We never call dsp_process()
 * because pcmbuf_insert is a sink; the DSP stays passive. */
static void bench_cb_configure(int setting, intptr_t value)
{
    dsp_configure(bench_ci.dsp, setting, value);
}

static long bench_cb_get_command(intptr_t *param)
{
    (void)param;
    yield();
    return bench_codec_action;
}

static bool bench_cb_loop_track(void)
{
    return false;
}

static void bench_cb_strip_filesize(off_t value)
{
    bench_ci.filesize = value;
}

/* ------------------------------------------------------------------ *
 *  ci initialisation.                                                *
 * ------------------------------------------------------------------ */

static void bench_init_ci(void)
{
    memset(&bench_ci, 0, sizeof(bench_ci));

    bench_ci.dsp = dsp_get_config(CODEC_IDX_AUDIO);

    /* Use the production codec_get_buffer_callback — returns leftover
     * codecbuf space after the codec binary is loaded, identical to what
     * production playback hands codecs (apps/codec_thread.c:723). */
    bench_ci.codec_get_buffer = codec_get_buffer_callback;
    bench_ci.pcmbuf_insert    = bench_cb_pcmbuf_insert;
    bench_ci.set_elapsed      = bench_cb_set_elapsed;
    bench_ci.read_filebuf     = bench_cb_read_filebuf;
    bench_ci.request_buffer   = bench_cb_request_buffer;
    bench_ci.advance_buffer   = bench_cb_advance_buffer;
    bench_ci.seek_buffer      = bench_cb_seek_buffer;
    bench_ci.seek_complete    = bench_cb_seek_complete;
    bench_ci.set_offset       = bench_cb_set_offset;
    bench_ci.configure        = bench_cb_configure;
    bench_ci.get_command      = bench_cb_get_command;
    bench_ci.loop_track       = bench_cb_loop_track;
    bench_ci.strip_filesize   = bench_cb_strip_filesize;

#if defined(ARM_NEED_DIV0)
    bench_ci.__div0 = __div0;
#endif
    bench_ci.sleep = sleep;
    bench_ci.yield = yield;

#if NUM_CORES > 1
    bench_ci.create_thread    = create_thread;
    bench_ci.thread_thaw      = thread_thaw;
    bench_ci.thread_wait      = thread_wait;
    bench_ci.semaphore_init   = semaphore_init;
    bench_ci.semaphore_wait   = semaphore_wait;
    bench_ci.semaphore_release= semaphore_release;
#endif

    bench_ci.commit_dcache         = commit_dcache;
    bench_ci.commit_discard_dcache = commit_discard_dcache;
    bench_ci.commit_discard_idcache= commit_discard_idcache;

    bench_ci.strcpy  = strcpy;
    bench_ci.strlen  = strlen;
    bench_ci.strcmp  = strcmp;
    bench_ci.strcat  = strcat;
    bench_ci.memset  = memset;
    bench_ci.memcpy  = memcpy;
    bench_ci.memmove = memmove;
    bench_ci.memcmp  = memcmp;
    bench_ci.memchr  = memchr;
#if defined(DEBUG) || defined(SIMULATOR)
    bench_ci.debugf  = debugf;
#endif
#ifdef ROCKBOX_HAS_LOGF
    bench_ci.logf    = logf;
#endif
    bench_ci.qsort   = (void *)qsort;
}

/* ------------------------------------------------------------------ *
 *  Codec-thread worker (runs ON codec thread via                     *
 *  codec_thread_do_callback). Mirrors apps/plugins/test_codec.c      *
 *  codec_thread() at lines 607-629.                                  *
 * ------------------------------------------------------------------ */

static void bench_codec_thread_worker(void)
{
    const char *codec_root_fn = bench_ci.id3 ?
        get_codec_filename(bench_ci.id3->codectype) : NULL;
    int rc = -1;

    if (codec_root_fn != NULL)
    {
        rc = codec_load_file(codec_root_fn, &bench_ci);
        if (rc >= 0)
            rc = codec_run_proc();
        codec_close();
    }

    bench_endtick = current_tick - bench_rebuffer_ticks;
    bench_run_rc  = rc;
    commit_dcache();
    bench_codec_playing = false;
}

/* ------------------------------------------------------------------ *
 *  Public entry: run one codec, blocking.                            *
 * ------------------------------------------------------------------ */

int wf_codec_bench_run_one(int slot,
                           struct wf_bench_result *out,
                           volatile bool *cancel)
{
    const char *path;
    long starttick;
    bool user_cancelled = false;

    if (!out || slot < 0 || slot >= WF_BENCH_CORPUS_COUNT)
        return -1;

    memset(out, 0, sizeof(*out));
    out->afmt = bench_corpus[slot].afmt;

    path = wf_codec_bench_path(slot);
    bench_fd = open(path, O_RDONLY);
    if (bench_fd < 0)
    {
        out->status = WF_BENCH_SKIP_NO_FILE;
        return 0;
    }

    bench_filesize = filesize(bench_fd);
    out->filesize_bytes = (uint32_t)bench_filesize;

    memset(&bench_id3, 0, sizeof(bench_id3));
    if (!get_metadata(&bench_id3, bench_fd, path))
    {
        out->status = WF_BENCH_ERR_METADATA;
        close(bench_fd);
        bench_fd = -1;
        return 0;
    }
    out->duration_ms = (uint32_t)bench_id3.length;

    /* Force codectype to match our corpus expectation. Some metadata
     * readers (mp4) may return AAC for an ALAC file or vice versa if the
     * underlying container doesn't unambiguously match; we trust the
     * corpus's named slot over the readback for codec dispatch. */
    bench_id3.codectype = bench_corpus[slot].afmt;

    /* Preload the first window. */
    bench_curr_offset    = 0;
    bench_curr_bufsize   = 0;
    bench_rebuffer_ticks = 0;
    if (bench_fill_buffer(0) < 0)
    {
        out->status = WF_BENCH_SKIP_NO_FILE;
        close(bench_fd);
        bench_fd = -1;
        return 0;
    }

    bench_init_ci();
    bench_ci.filesize = bench_filesize;
    bench_ci.id3      = &bench_id3;
    bench_ci.curpos   = 0;

    /* Reset DSP state before each codec — matches test_codec lines 700-702.
     * Cheap insurance; codecs assume a fresh DSP per track. */
    dsp_configure(bench_ci.dsp, DSP_RESET, 0);
    dsp_configure(bench_ci.dsp, DSP_FLUSH, 0);

    bench_run_rc        = 0;
    bench_codec_action  = CODEC_ACTION_NULL;
    bench_codec_playing = true;
    commit_dcache();

    starttick = current_tick;
    codec_thread_do_callback(bench_codec_thread_worker, NULL);

    while (bench_codec_playing)
    {
        sleep(HZ / 10);              /* 100 ms — keeps yield + drain time bounded */
        if (cancel && *cancel)
        {
            user_cancelled = true;
            bench_codec_action = CODEC_ACTION_HALT;
            commit_dcache();
        }
    }

    /* Wait for codec thread to fully unwind — same idiom as test_codec:732. */
    codec_thread_do_callback(NULL, NULL);

    close(bench_fd);
    bench_fd = -1;

    out->decode_ticks = (uint32_t)(bench_endtick - starttick);

    if (user_cancelled)
    {
        out->status = WF_BENCH_ABORTED;
    }
    else if (bench_run_rc < 0)
    {
        out->status = (bench_run_rc == -1 && bench_ci.curpos == 0)
                      ? WF_BENCH_ERR_LOAD : WF_BENCH_ERR_RUN;
    }
    else
    {
        out->status = WF_BENCH_OK;
    }

    if (out->status == WF_BENCH_OK && out->decode_ticks > 0 &&
        out->duration_ms > 0)
    {
        /* realtime_pct_x100 = (duration_ms * 100) / (ticks * 10)
         *                  = duration_ms * 10 / ticks
         * but to keep precision, mirror test_codec:758 verbatim:
         *   speed = (duration_ms / 10) * 10000 / ticks
         *         = duration_cs * 10000 / ticks
         * which yields units of 1/100 % — exactly realtime_pct_x100. */
        uint32_t duration_cs = out->duration_ms / 10;
        out->realtime_pct_x100 =
            (uint32_t)(((uint64_t)duration_cs * 10000ull) / out->decode_ticks);

        if (out->realtime_pct_x100 > 0)
        {
            /* mhz_x100 = cpu_frequency / realtime_pct_x100. cpu_frequency
             * is in Hz, realtime_pct_x100 is %x100 — Hz/(%x100) ≈ MHz*100
             * since the denominators (1e6 / 1e4) cancel correctly. */
            out->mhz_x100 = (uint32_t)cpu_frequency / out->realtime_pct_x100;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 *  Report writer.                                                    *
 * ------------------------------------------------------------------ */

static const char *bench_status_name(enum wf_bench_status s)
{
    switch (s)
    {
        case WF_BENCH_OK:           return "OK";
        case WF_BENCH_SKIP_NO_FILE: return "SKIP_NO_FILE";
        case WF_BENCH_ERR_METADATA: return "ERR_METADATA";
        case WF_BENCH_ERR_LOAD:     return "ERR_LOAD";
        case WF_BENCH_ERR_RUN:      return "ERR_RUN";
        case WF_BENCH_ABORTED:      return "ABORTED";
        default:                    return "?";
    }
}

int wf_codec_bench_write_report(const struct wf_bench_result *res, size_t n)
{
    const char *path = ROCKBOX_DIR "/wf_codec_bench.txt";
    int fd;
    size_t i;

    fd = creat(path, 0666);
    if (fd < 0)
        return -1;

    fdprintf(fd, "# wf_codec_bench v1\n");
    fdprintf(fd, "ticks_hz=%d\n", HZ);
    fdprintf(fd, "cpu_frequency_hz=%ld\n", (long)cpu_frequency);

    for (i = 0; i < n; i++)
    {
        const struct wf_bench_result *r = &res[i];
        int slot = -1;
        size_t k;

        for (k = 0; k < WF_BENCH_CORPUS_COUNT; k++)
        {
            if (bench_corpus[k].afmt == r->afmt) { slot = (int)k; break; }
        }

        fdprintf(fd,
            "format=%s status=%s size=%lu duration_ms=%lu "
            "decode_ticks=%lu realtime_pct=%lu.%02lu mhz=%lu.%02lu\n",
            (slot >= 0) ? bench_corpus[slot].display : "?",
            bench_status_name(r->status),
            (unsigned long)r->filesize_bytes,
            (unsigned long)r->duration_ms,
            (unsigned long)r->decode_ticks,
            (unsigned long)(r->realtime_pct_x100 / 100),
            (unsigned long)(r->realtime_pct_x100 % 100),
            (unsigned long)(r->mhz_x100 / 100),
            (unsigned long)(r->mhz_x100 % 100));
    }

    close(fd);
    return 0;
}

#endif /* WAVEFORM_TELEMETRY_BENCH */
