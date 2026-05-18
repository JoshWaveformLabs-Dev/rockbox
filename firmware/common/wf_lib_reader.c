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

/* Waveform OS Stage 3 D3 — WFLIB v1.0 reader.
 *
 * Spec: docs/research/WFLIB_FORMAT_v1.md. Struct layouts:
 * firmware/include/wf_lib.h. Public API: firmware/include/wf_lib_reader.h.
 *
 * Invariants:
 *   - Process-singleton g_lib holds all reader state.
 *   - All scratch buffers are buflib handles allocated at mount, freed at
 *     unmount. We pin BEFORE every read() and unpin IMMEDIATELY after, so
 *     buflib may compact other blocks during I/O but our buffer stays put.
 *   - pin/unpin pairs never span a function-call boundary that yields.
 *     The only yieldable call between pin and unpin is read() itself, and
 *     the pin is exactly there to keep our buffer at a fixed address
 *     through any concurrent compaction.
 *   - Any §18 validation failure → unmount path frees everything and
 *     leaves state == INVALID. Callers treat INVALID as "no library".
 *   - Working-set budget: header (resident in BSS), section table (~176 B),
 *     two 4 KiB scratch buffers = ~8 KiB total. Spec §22 cap is 96 KiB;
 *     remainder reserved for D4 LRU caches.
 */

#include "config.h"

#ifdef WAVEFORM_WFLIB

#include "wf_lib_reader.h"
#include "wf_lib.h"
#include "file.h"
#include "crc32.h"
#include "core_alloc.h"
#include "mutex.h"
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* ----- module state --------------------------------------------------- */

#define WFLIB_SCRATCH_BYTES   4096u
#define WFLIB_MAX_PATH        256
#define WFLIB_MAX_BUCKET_STR  1024  /* spec §9: max 1023 B/string */

#define WFLIB_HDR_CRC_RANGE1_OFF   0
#define WFLIB_HDR_CRC_RANGE1_LEN   12
#define WFLIB_HDR_CRC_RANGE2_OFF   16
#define WFLIB_HDR_CRC_RANGE2_LEN   48

static struct {
    enum wf_lib_state  state;
    int                fd;
    struct wflib_header hdr;
    char               mounted_path[WFLIB_MAX_PATH];

    /* buflib handles (each -1 when unallocated). */
    int sec_table_handle;       /* section table copy (≤ 11 * 16 B) */
    int scratch_pool_handle;    /* 4 KiB scratch for STRING/PATH buckets */
    int scratch_page_handle;    /* 4 KiB scratch for track/sort/dict reads */

    /* per-section file offsets cached from section table at mount. */
    uint32_t off_string_pool;
    uint32_t off_path_pool;
    uint32_t off_dict_artist;
    uint32_t off_dict_album;
    uint32_t off_dict_genre;
    uint32_t off_dict_composer;
    uint32_t off_track_records;
    uint32_t off_sort_title;
    uint32_t off_sort_album_track;
    uint32_t off_sort_artist_album;
    uint32_t off_sort_composer;

    uint32_t len_string_pool;
    uint32_t len_path_pool;

    /* dict entry counts (validated to fit in u16 at mount). */
    uint32_t artist_count;
    uint32_t album_count;
    uint32_t genre_count;
    uint32_t composer_count;

    /* sort entry counts (may be < hdr.track_count for composer sort). */
    uint32_t sort_count_title;
    uint32_t sort_count_album_track;
    uint32_t sort_count_artist_album;
    uint32_t sort_count_composer;

    /* string + path pool header values cached for resolve. */
    uint32_t string_pool_bucket_count;
    uint32_t string_pool_string_count;
    uint32_t string_pool_bucket_table_abs;  /* absolute file offset */

    uint32_t path_pool_bucket_count;
    uint32_t path_pool_string_count;
    uint32_t path_pool_bucket_table_abs;
} g_lib;

/* D4.1 thread-safety: every public wf_lib_* function takes this mutex
 * coarsely. Reads are short, the API is small, and the mutex protects
 * g_lib + buflib handles against concurrent unmount/remount on the
 * browser/playlist hot paths. Internal helpers (read_at, parse_section_
 * table, wflib_cleanup, …) are unlocked and only called from already-
 * locked public entry points. */
static struct mutex g_lib_mutex;
static bool         g_lib_inited;

/* ----- helpers -------------------------------------------------------- */

static uint16_t rd_u16(const uint8_t *p) {
    /* avoid unaligned hardware load on ARM by going through memcpy */
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}

static uint32_t rd_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* Compute the spec §6 CRC over header bytes [0..12) ∪ [16..64).
 * Rockbox crc_32r uses the reflected polynomial 0xEDB88320 (same as zlib)
 * and returns the raw register state; ISO-HDLC requires init 0xFFFFFFFF
 * and xorout 0xFFFFFFFF. */
static uint32_t wflib_header_crc(const uint8_t *hdr64) {
    uint32_t c = crc_32r(hdr64 + WFLIB_HDR_CRC_RANGE1_OFF,
                         WFLIB_HDR_CRC_RANGE1_LEN, 0xFFFFFFFFu);
    c = crc_32r(hdr64 + WFLIB_HDR_CRC_RANGE2_OFF,
                WFLIB_HDR_CRC_RANGE2_LEN, c);
    return c ^ 0xFFFFFFFFu;
}

/* Seek+read; returns 0 on full read, -1 on any error or short read.
 * read() may yield on real hardware. Callers that need buflib data
 * must hold a pin over this entire call. */
static int read_at(off_t off, void *buf, size_t n) {
    if (g_lib.fd < 0) return -1;
    if (lseek(g_lib.fd, off, SEEK_SET) != off) return -1;
    ssize_t got = read(g_lib.fd, buf, n);
    return (got == (ssize_t)n) ? 0 : -1;
}

/* Pin handle, read n bytes from file at off into it, unpin. n MUST be ≤
 * the handle's allocated size — caller enforces. */
static int read_into_pinned(int handle, off_t off, size_t n) {
    void *buf = core_get_data_pinned(handle);
    int rc = read_at(off, buf, n);
    core_put_data_pinned(buf);
    return rc;
}

/* Free any handle that was allocated. Idempotent. */
static void free_handle(int *h) {
    if (*h >= 0) {
        core_free(*h);
        *h = -1;
    }
}

/* Reset module state to UNMOUNTED, releasing every resource. */
static void wflib_cleanup(void) {
    if (g_lib.fd >= 0) {
        close(g_lib.fd);
        g_lib.fd = -1;
    }
    free_handle(&g_lib.sec_table_handle);
    free_handle(&g_lib.scratch_pool_handle);
    free_handle(&g_lib.scratch_page_handle);
    /* preserve g_lib.state — caller decides INVALID vs UNMOUNTED */
    g_lib.mounted_path[0] = '\0';
    memset(&g_lib.hdr, 0, sizeof g_lib.hdr);
    g_lib.off_string_pool = 0;
    g_lib.off_path_pool = 0;
    g_lib.off_dict_artist = 0;
    g_lib.off_dict_album = 0;
    g_lib.off_dict_genre = 0;
    g_lib.off_dict_composer = 0;
    g_lib.off_track_records = 0;
    g_lib.off_sort_title = 0;
    g_lib.off_sort_album_track = 0;
    g_lib.off_sort_artist_album = 0;
    g_lib.off_sort_composer = 0;
    g_lib.len_string_pool = 0;
    g_lib.len_path_pool = 0;
    g_lib.artist_count = 0;
    g_lib.album_count = 0;
    g_lib.genre_count = 0;
    g_lib.composer_count = 0;
    g_lib.sort_count_title = 0;
    g_lib.sort_count_album_track = 0;
    g_lib.sort_count_artist_album = 0;
    g_lib.sort_count_composer = 0;
    g_lib.string_pool_bucket_count = 0;
    g_lib.string_pool_string_count = 0;
    g_lib.string_pool_bucket_table_abs = 0;
    g_lib.path_pool_bucket_count = 0;
    g_lib.path_pool_string_count = 0;
    g_lib.path_pool_bucket_table_abs = 0;
}

/* ----- per-section header validation --------------------------------- */

/* Read 8-byte dict header at offset off; validate entry_size, capture
 * entry_count. Returns 0 on success, -1 on error. */
static int read_dict_header(uint32_t off, uint32_t sec_len,
                            uint16_t expected_entry_size,
                            uint32_t *out_count)
{
    uint8_t buf[8];
    if (sec_len < 8) return -1;
    if (read_at((off_t)off, buf, 8) != 0) return -1;
    uint32_t count = rd_u32(&buf[0]);
    uint16_t esize = rd_u16(&buf[4]);
    uint16_t resv  = rd_u16(&buf[6]);
    if (resv != 0) return -1;
    if (esize != expected_entry_size) return -1;
    if (count > 65535u) return -1;                       /* spec §11 cap */
    if ((uint64_t)8 + (uint64_t)count * esize > sec_len) return -1;
    *out_count = count;
    return 0;
}

/* Read 8-byte sort/track header; validate record_size, capture count. */
static int read_count_header(uint32_t off, uint32_t sec_len,
                             uint16_t expected_record_size,
                             uint32_t *out_count)
{
    uint8_t buf[8];
    if (sec_len < 8) return -1;
    if (read_at((off_t)off, buf, 8) != 0) return -1;
    uint32_t count = rd_u32(&buf[0]);
    uint16_t rsize = rd_u16(&buf[4]);
    uint16_t resv  = rd_u16(&buf[6]);
    if (resv != 0) return -1;
    if (rsize != expected_record_size) return -1;
    if ((uint64_t)8 + (uint64_t)count * rsize > sec_len) return -1;
    *out_count = count;
    return 0;
}

/* Read 16-byte string-pool header; capture bucket_count, string_count,
 * absolute bucket-table offset. Validates internal consistency. */
static int read_pool_header(uint32_t sec_off, uint32_t sec_len,
                            uint32_t *out_bucket_count,
                            uint32_t *out_string_count,
                            uint32_t *out_bucket_table_abs)
{
    uint8_t buf[16];
    if (sec_len < 16) return -1;
    if (read_at((off_t)sec_off, buf, 16) != 0) return -1;
    uint32_t bcount  = rd_u32(&buf[0]);
    uint32_t scount  = rd_u32(&buf[4]);
    uint32_t btable  = rd_u32(&buf[8]);   /* offset within section */
    uint32_t resv    = rd_u32(&buf[12]);
    if (resv != 0) return -1;
    /* Empty pool is legal: bcount == 0 ⇒ scount == 0 and no table. */
    if (bcount == 0) {
        if (scount != 0) return -1;
        *out_bucket_count = 0;
        *out_string_count = 0;
        *out_bucket_table_abs = 0;
        return 0;
    }
    if (scount == 0) return -1;
    if (scount > (uint64_t)bcount * 16u) return -1;
    if (scount <= (uint64_t)(bcount - 1) * 16u) return -1;   /* last bucket must hold ≥ 1 */
    /* bucket table bounds */
    if ((uint64_t)btable + (uint64_t)bcount * 4u > sec_len) return -1;
    *out_bucket_count = bcount;
    *out_string_count = scount;
    *out_bucket_table_abs = sec_off + btable;
    return 0;
}

/* ----- mount sequence (spec §18) -------------------------------------- */

/* Reads the section table from the section-table handle and indexes
 * every required type_id into g_lib.off_*. Validates per-entry bounds,
 * flags, reserved, and required-section presence (§18 steps 10–14). */
static int parse_section_table(void) {
    uint16_t count = g_lib.hdr.section_table_count;

    /* track which required type_ids we've seen */
    uint8_t seen[12] = {0};    /* indices 1..11 used for required types */
    int duplicate = 0;

    void *tbl = core_get_data_pinned(g_lib.sec_table_handle);
    const uint8_t *entries = (const uint8_t *)tbl;
    int rc = -1;

    for (uint16_t i = 0; i < count; i++) {
        const uint8_t *e = entries + i * 16;
        uint32_t type_id = rd_u32(e + 0);
        uint32_t off     = rd_u32(e + 4);
        uint32_t len     = rd_u32(e + 8);
        uint16_t flags   = rd_u16(e + 12);
        uint16_t resv    = rd_u16(e + 14);

        if (resv != 0) goto fail;
        if (flags & ~(uint16_t)WFLIB_SECFLAG_REQUIRED) goto fail;

        /* bounds: offset + length ≤ file_length (spec §18.10) */
        if ((uint64_t)off + (uint64_t)len > g_lib.hdr.file_length) goto fail;

        /* required-section unknown type → reject (§18.13) */
        int is_core = (type_id >= 0x0001 && type_id <= 0x000B);
        if (!is_core && (flags & WFLIB_SECFLAG_REQUIRED)) goto fail;
        if (!is_core) continue;   /* unknown non-required: silently skip */

        /* Duplicate detection. */
        if (seen[type_id]) { duplicate = 1; goto fail; }
        seen[type_id] = 1;

        switch (type_id) {
        case WFLIB_SEC_STRING_POOL:
            g_lib.off_string_pool = off; g_lib.len_string_pool = len; break;
        case WFLIB_SEC_PATH_POOL:
            g_lib.off_path_pool = off; g_lib.len_path_pool = len; break;
        case WFLIB_SEC_DICT_ARTIST:
            g_lib.off_dict_artist = off; break;
        case WFLIB_SEC_DICT_ALBUM:
            g_lib.off_dict_album = off; break;
        case WFLIB_SEC_DICT_GENRE:
            g_lib.off_dict_genre = off; break;
        case WFLIB_SEC_DICT_COMPOSER:
            g_lib.off_dict_composer = off; break;
        case WFLIB_SEC_TRACK_RECORDS:
            g_lib.off_track_records = off; break;
        case WFLIB_SEC_SORT_TITLE:
            g_lib.off_sort_title = off; break;
        case WFLIB_SEC_SORT_ALBUM_TRACK:
            g_lib.off_sort_album_track = off; break;
        case WFLIB_SEC_SORT_ARTIST_ALBUM:
            g_lib.off_sort_artist_album = off; break;
        case WFLIB_SEC_SORT_COMPOSER_ALBUM_TRACK:
            g_lib.off_sort_composer = off; break;
        default:
            break;
        }
    }

    /* All v1.0 core sections required (§18.11). */
    for (int t = WFLIB_SEC_STRING_POOL; t <= WFLIB_SEC_SORT_COMPOSER_ALBUM_TRACK; t++) {
        if (!seen[t]) goto fail;
    }

    rc = 0;
fail:
    core_put_data_pinned(tbl);
    (void)duplicate;
    return rc;
}

/* The big mount sequence. Returns one of the three states. */
static enum wf_lib_state wflib_mount_internal(const char *path) {
    /* Step 2: open */
    g_lib.fd = open(path, O_RDONLY);
    if (g_lib.fd < 0) {
        /* File missing or unreadable — NOT validation failure. */
        g_lib.state = WF_LIB_STATE_UNMOUNTED;
        return WF_LIB_STATE_UNMOUNTED;
    }

    /* From here, any failure → INVALID. */
    g_lib.state = WF_LIB_STATE_INVALID;

    /* Step 18.1: file ≥ 64 bytes */
    off_t fsize = filesize(g_lib.fd);
    if (fsize < 64) goto invalid;

    /* Step 3: read header into a local buffer, then validate. */
    uint8_t hdr_buf[64];
    if (read_at(0, hdr_buf, 64) != 0) goto invalid;

    /* Step 18.2: magic */
    if (memcmp(hdr_buf, WFLIB_MAGIC_STR, 4) != 0) goto invalid;
    /* Step 18.3: format_major */
    if (hdr_buf[4] != WFLIB_FORMAT_MAJOR) goto invalid;
    /* Step 18.4: (reader_maj, reader_min) ≥ (min_maj, min_min) */
    uint8_t min_maj = hdr_buf[6], min_min = hdr_buf[7];
    if (min_maj > WFLIB_READER_MAJOR) goto invalid;
    if (min_maj == WFLIB_READER_MAJOR && min_min > WFLIB_READER_MINOR) goto invalid;

    /* Decode header fields. */
    uint32_t file_length = rd_u32(hdr_buf + 8);
    uint32_t hdr_crc     = rd_u32(hdr_buf + 12);
    uint32_t st_off      = rd_u32(hdr_buf + 16);
    uint16_t st_count    = rd_u16(hdr_buf + 20);
    uint16_t st_esize    = rd_u16(hdr_buf + 22);

    /* Step 18.5: file_length matches FS size */
    if ((off_t)file_length != fsize) goto invalid;
    /* Step 18.9: entry size frozen at 16 in v1.x */
    if (st_esize != 16) goto invalid;
    /* Step 18.6: header CRC */
    if (wflib_header_crc(hdr_buf) != hdr_crc) goto invalid;
    /* Step 18.7: all 16 reserved bytes zero */
    for (int i = 48; i < 64; i++) if (hdr_buf[i] != 0) goto invalid;
    /* Step 18.8: section-table bounds */
    if ((uint64_t)st_off + (uint64_t)st_count * 16u > file_length) goto invalid;
    /* Sanity: section table count > 0 and small enough to fit in scratch
     * range. 0xFFFF entries × 16 B = 1 MiB; in practice ≤ 11 entries. */
    if (st_count == 0) goto invalid;

    /* Cache the validated header into g_lib.hdr via memcpy (packed struct,
     * caller's natural alignment is fine). */
    memcpy(&g_lib.hdr, hdr_buf, 64);

    /* Step 12: allocate + read section table. */
    g_lib.sec_table_handle = core_alloc((size_t)st_count * 16u);
    if (g_lib.sec_table_handle < 0) goto invalid;
    if (read_into_pinned(g_lib.sec_table_handle, (off_t)st_off,
                         (size_t)st_count * 16u) != 0) goto invalid;

    /* Steps 18.10–18.13: walk section table, capture offsets, verify required. */
    if (parse_section_table() != 0) goto invalid;

    /* Validate each section header (track_count consistency, entry_size,
     * record_size, reserved fields). */
    uint32_t tc = 0;
    if (read_count_header(g_lib.off_track_records,
                          g_lib.hdr.file_length - g_lib.off_track_records,
                          32, &tc) != 0) goto invalid;
    if (tc != g_lib.hdr.track_count) goto invalid;

    if (read_count_header(g_lib.off_sort_title,
                          g_lib.hdr.file_length - g_lib.off_sort_title,
                          4, &g_lib.sort_count_title) != 0) goto invalid;
    if (read_count_header(g_lib.off_sort_album_track,
                          g_lib.hdr.file_length - g_lib.off_sort_album_track,
                          4, &g_lib.sort_count_album_track) != 0) goto invalid;
    if (read_count_header(g_lib.off_sort_artist_album,
                          g_lib.hdr.file_length - g_lib.off_sort_artist_album,
                          4, &g_lib.sort_count_artist_album) != 0) goto invalid;
    if (read_count_header(g_lib.off_sort_composer,
                          g_lib.hdr.file_length - g_lib.off_sort_composer,
                          4, &g_lib.sort_count_composer) != 0) goto invalid;

    /* Title/album_track/artist_album sorts MUST equal track_count
     * (spec §16 — only composer sort may omit). */
    if (g_lib.sort_count_title != g_lib.hdr.track_count) goto invalid;
    if (g_lib.sort_count_album_track != g_lib.hdr.track_count) goto invalid;
    if (g_lib.sort_count_artist_album != g_lib.hdr.track_count) goto invalid;
    if (g_lib.sort_count_composer > g_lib.hdr.track_count) goto invalid;

    if (read_dict_header(g_lib.off_dict_artist,
                         g_lib.hdr.file_length - g_lib.off_dict_artist,
                         8, &g_lib.artist_count) != 0) goto invalid;
    if (read_dict_header(g_lib.off_dict_album,
                         g_lib.hdr.file_length - g_lib.off_dict_album,
                         16, &g_lib.album_count) != 0) goto invalid;
    if (read_dict_header(g_lib.off_dict_genre,
                         g_lib.hdr.file_length - g_lib.off_dict_genre,
                         8, &g_lib.genre_count) != 0) goto invalid;
    if (read_dict_header(g_lib.off_dict_composer,
                         g_lib.hdr.file_length - g_lib.off_dict_composer,
                         8, &g_lib.composer_count) != 0) goto invalid;
    /* Each dict MUST have at least the "Unknown" entry at index 0. */
    if (g_lib.artist_count == 0) goto invalid;
    if (g_lib.album_count == 0) goto invalid;
    if (g_lib.genre_count == 0) goto invalid;
    if (g_lib.composer_count == 0) goto invalid;

    /* Pool headers. */
    if (read_pool_header(g_lib.off_string_pool, g_lib.len_string_pool,
                         &g_lib.string_pool_bucket_count,
                         &g_lib.string_pool_string_count,
                         &g_lib.string_pool_bucket_table_abs) != 0) goto invalid;
    if (read_pool_header(g_lib.off_path_pool, g_lib.len_path_pool,
                         &g_lib.path_pool_bucket_count,
                         &g_lib.path_pool_string_count,
                         &g_lib.path_pool_bucket_table_abs) != 0) goto invalid;

    /* Allocate the two scratch buffers. */
    g_lib.scratch_pool_handle = core_alloc(WFLIB_SCRATCH_BYTES);
    if (g_lib.scratch_pool_handle < 0) goto invalid;
    g_lib.scratch_page_handle = core_alloc(WFLIB_SCRATCH_BYTES);
    if (g_lib.scratch_page_handle < 0) goto invalid;

    g_lib.state = WF_LIB_STATE_MOUNTED;
    return WF_LIB_STATE_MOUNTED;

invalid:
    wflib_cleanup();
    g_lib.state = WF_LIB_STATE_INVALID;
    return WF_LIB_STATE_INVALID;
}

/* ----- public API ----------------------------------------------------- */

void wf_lib_init(void) {
    /* Idempotent. Called once at boot from apps/main.c. Guarded so a
     * stray second call cannot reset an in-use mutex. */
    if (g_lib_inited) return;
    mutex_init(&g_lib_mutex);
    g_lib_inited = true;
}

enum wf_lib_state wf_lib_mount(const char *path) {
    if (!path || !path[0]) return WF_LIB_STATE_UNMOUNTED;

    mutex_lock(&g_lib_mutex);
    enum wf_lib_state ret;

    /* Idempotent / remount handling. */
    if (g_lib.state == WF_LIB_STATE_MOUNTED &&
        strncmp(g_lib.mounted_path, path, WFLIB_MAX_PATH - 1) == 0) {
        ret = WF_LIB_STATE_MOUNTED;
        goto out;
    }
    wflib_cleanup();
    g_lib.state = WF_LIB_STATE_UNMOUNTED;

    /* Snapshot path before any I/O so remount checks work. Bounded copy. */
    size_t plen = strlen(path);
    if (plen >= WFLIB_MAX_PATH) { ret = WF_LIB_STATE_INVALID; goto out; }
    memcpy(g_lib.mounted_path, path, plen + 1);

    ret = wflib_mount_internal(path);
    if (ret != WF_LIB_STATE_MOUNTED) {
        g_lib.mounted_path[0] = '\0';
    }

out:
    mutex_unlock(&g_lib_mutex);
    return ret;
}

void wf_lib_unmount(void) {
    mutex_lock(&g_lib_mutex);
    wflib_cleanup();
    g_lib.state = WF_LIB_STATE_UNMOUNTED;
    mutex_unlock(&g_lib_mutex);
}

enum wf_lib_state wf_lib_state(void) {
    mutex_lock(&g_lib_mutex);
    enum wf_lib_state s = g_lib.state;
    mutex_unlock(&g_lib_mutex);
    return s;
}

#define WFLIB_LOCKED_U32(expr)                                       \
    do {                                                             \
        mutex_lock(&g_lib_mutex);                                    \
        uint32_t _v = (g_lib.state == WF_LIB_STATE_MOUNTED) ? (expr) : 0; \
        mutex_unlock(&g_lib_mutex);                                  \
        return _v;                                                   \
    } while (0)

#define WFLIB_LOCKED_U64(expr)                                       \
    do {                                                             \
        mutex_lock(&g_lib_mutex);                                    \
        uint64_t _v = (g_lib.state == WF_LIB_STATE_MOUNTED) ? (expr) : 0; \
        mutex_unlock(&g_lib_mutex);                                  \
        return _v;                                                   \
    } while (0)

uint32_t wf_lib_track_count(void)       { WFLIB_LOCKED_U32(g_lib.hdr.track_count); }
uint32_t wf_lib_total_duration_s(void)  { WFLIB_LOCKED_U32(g_lib.hdr.total_duration_s); }
uint64_t wf_lib_builder_id(void)        { WFLIB_LOCKED_U64(g_lib.hdr.builder_id); }
uint64_t wf_lib_source_mtime(void)      { WFLIB_LOCKED_U64(g_lib.hdr.source_mtime); }
uint32_t wf_lib_artist_count(void)      { WFLIB_LOCKED_U32(g_lib.artist_count); }
uint32_t wf_lib_album_count(void)       { WFLIB_LOCKED_U32(g_lib.album_count); }
uint32_t wf_lib_genre_count(void)       { WFLIB_LOCKED_U32(g_lib.genre_count); }
uint32_t wf_lib_composer_count(void)    { WFLIB_LOCKED_U32(g_lib.composer_count); }

#undef WFLIB_LOCKED_U32
#undef WFLIB_LOCKED_U64

int wf_lib_get_track(uint32_t track_id, struct wflib_track_record *out) {
    if (!out) return -1;
    mutex_lock(&g_lib_mutex);
    int rc = -1;
    if (g_lib.state != WF_LIB_STATE_MOUNTED) goto out;
    if (track_id >= g_lib.hdr.track_count) goto out;
    off_t off = (off_t)g_lib.off_track_records + 8 + (off_t)track_id * 32;
    if (read_at(off, out, 32) != 0) goto out;
    rc = 0;
out:
    mutex_unlock(&g_lib_mutex);
    return rc;
}

/* Caller MUST hold g_lib_mutex. */
static int dict_get_generic(uint32_t base, uint32_t count, uint16_t id,
                            void *out, size_t entry_size) {
    if (!out) return -1;
    if (g_lib.state != WF_LIB_STATE_MOUNTED) return -1;
    if ((uint32_t)id >= count) return -1;
    off_t off = (off_t)base + 8 + (off_t)id * entry_size;
    if (read_at(off, out, entry_size) != 0) return -1;
    return 0;
}

int wf_lib_get_artist(uint16_t id, struct wflib_artist_entry *out) {
    mutex_lock(&g_lib_mutex);
    int rc = dict_get_generic(g_lib.off_dict_artist, g_lib.artist_count, id, out, 8);
    mutex_unlock(&g_lib_mutex);
    return rc;
}
int wf_lib_get_album(uint16_t id, struct wflib_album_entry *out) {
    mutex_lock(&g_lib_mutex);
    int rc = dict_get_generic(g_lib.off_dict_album, g_lib.album_count, id, out, 16);
    mutex_unlock(&g_lib_mutex);
    return rc;
}
int wf_lib_get_genre(uint16_t id, struct wflib_genre_entry *out) {
    mutex_lock(&g_lib_mutex);
    int rc = dict_get_generic(g_lib.off_dict_genre, g_lib.genre_count, id, out, 8);
    mutex_unlock(&g_lib_mutex);
    return rc;
}
int wf_lib_get_composer(uint16_t id, struct wflib_composer_entry *out) {
    mutex_lock(&g_lib_mutex);
    int rc = dict_get_generic(g_lib.off_dict_composer, g_lib.composer_count, id, out, 8);
    mutex_unlock(&g_lib_mutex);
    return rc;
}

static int sort_resolve(uint32_t sort_type,
                        uint32_t *out_off, uint32_t *out_count) {
    switch (sort_type) {
    case WFLIB_SEC_SORT_TITLE:
        *out_off = g_lib.off_sort_title;
        *out_count = g_lib.sort_count_title;
        return 0;
    case WFLIB_SEC_SORT_ALBUM_TRACK:
        *out_off = g_lib.off_sort_album_track;
        *out_count = g_lib.sort_count_album_track;
        return 0;
    case WFLIB_SEC_SORT_ARTIST_ALBUM:
        *out_off = g_lib.off_sort_artist_album;
        *out_count = g_lib.sort_count_artist_album;
        return 0;
    case WFLIB_SEC_SORT_COMPOSER_ALBUM_TRACK:
        *out_off = g_lib.off_sort_composer;
        *out_count = g_lib.sort_count_composer;
        return 0;
    default:
        return -1;
    }
}

uint32_t wf_lib_sort_track_at(uint32_t sort_type, uint32_t index) {
    mutex_lock(&g_lib_mutex);
    uint32_t ret = UINT32_MAX;
    if (g_lib.state != WF_LIB_STATE_MOUNTED) goto out;
    uint32_t base = 0, count = 0;
    if (sort_resolve(sort_type, &base, &count) != 0) goto out;
    if (index >= count) goto out;
    uint8_t buf[4];
    off_t off = (off_t)base + 8 + (off_t)index * 4;
    if (read_at(off, buf, 4) != 0) goto out;
    uint32_t tid = rd_u32(buf);
    if (tid >= g_lib.hdr.track_count) goto out;
    ret = tid;
out:
    mutex_unlock(&g_lib_mutex);
    return ret;
}

uint32_t wf_lib_sort_count(uint32_t sort_type) {
    mutex_lock(&g_lib_mutex);
    uint32_t ret = 0;
    if (g_lib.state != WF_LIB_STATE_MOUNTED) goto out;
    uint32_t base = 0, count = 0;
    if (sort_resolve(sort_type, &base, &count) != 0) goto out;
    ret = count;
out:
    mutex_unlock(&g_lib_mutex);
    return ret;
}

/* ----- string / path pool decode ------------------------------------- */

/* Walk back from `end` to the largest UTF-8-character boundary ≤ end.
 * Returns the safe truncation length. Used to avoid emitting a partial
 * multi-byte sequence at buflen cutoff. UTF-8 continuation bytes have
 * the high bits 10xxxxxx (0x80..0xBF); leaders are 0xC0..0xFF. */
static size_t utf8_truncate_at(const uint8_t *buf, size_t end) {
    if (end == 0) return 0;
    /* Count continuation bytes we walk past on the way to a leader/ASCII. */
    size_t p = end;
    size_t n_cont = 0;
    while (p > 0 && (buf[p - 1] & 0xC0) == 0x80) { p--; n_cont++; }
    if (p == 0) return 0;                 /* malformed: only continuations */
    uint8_t lead = buf[p - 1];
    if (lead < 0x80) {
        /* ASCII byte. ASCII has no continuations, so any continuations
         * walked past are garbage; drop them. */
        return n_cont == 0 ? end : p;
    }
    /* Multi-byte leader — determine how many continuations it expects. */
    size_t expected;
    if      ((lead & 0xE0) == 0xC0) expected = 1;
    else if ((lead & 0xF0) == 0xE0) expected = 2;
    else if ((lead & 0xF8) == 0xF0) expected = 3;
    else return p - 1;                    /* invalid leader byte */
    if (n_cont == expected) return end;   /* complete char — keep all */
    return p - 1;                          /* incomplete — drop leader */
}

/* Decode bucket bytes in scratch (n bytes starting at scratch[0]) to
 * recover string at intra_index. Writes up to buflen bytes into out.
 * Returns bytes written, or -1 on malformed bucket. */
static int decode_bucket_string(const uint8_t *scratch, size_t scratch_len,
                                uint32_t intra_index,
                                char *out, size_t buflen)
{
    /* Reusable in-bucket buffer for the previous string (max 1023 B
     * per spec §9). Allocated on stack — 1 KiB is acceptable for this
     * code path. */
    uint8_t prev[WFLIB_MAX_BUCKET_STR];
    size_t  prev_len = 0;

    if (scratch_len < 4) return -1;
    uint16_t bucket_byte_len = rd_u16(scratch + 0);
    if (bucket_byte_len > scratch_len) return -1;
    if (bucket_byte_len < 4) return -1;

    uint16_t first_len = rd_u16(scratch + 2);
    if (first_len > WFLIB_MAX_BUCKET_STR) return -1;
    size_t cur = 4;
    if (cur + first_len > bucket_byte_len) return -1;
    memcpy(prev, scratch + cur, first_len);
    prev_len = first_len;
    cur += first_len;

    /* If the caller wants string 0, prev currently holds it. */
    for (uint32_t i = 1; i <= intra_index; i++) {
        if (cur + 2 > bucket_byte_len) return -1;
        uint8_t shared = scratch[cur];
        uint8_t tail   = scratch[cur + 1];
        cur += 2;
        if (cur + tail > bucket_byte_len) return -1;
        if (shared > prev_len) return -1;
        if ((size_t)shared + tail > WFLIB_MAX_BUCKET_STR) return -1;
        /* in-place rebuild: keep first `shared` bytes of prev, append tail */
        memcpy(prev + shared, scratch + cur, tail);
        prev_len = (size_t)shared + tail;
        cur += tail;
    }

    /* Copy prev → out, truncating at UTF-8 boundary if needed. */
    if (buflen == 0) return 0;
    size_t copy = (prev_len <= buflen) ? prev_len : buflen;
    memcpy(out, prev, copy);
    if (copy < prev_len) {
        copy = utf8_truncate_at((const uint8_t *)out, copy);
    }
    return (int)copy;
}

/* Caller MUST hold g_lib_mutex. Pin/unpin of scratch handle is internal
 * to this function. */
static int resolve_pool(uint32_t string_offset, char *out, size_t buflen,
                        uint32_t bucket_count, uint32_t bucket_table_abs)
{
    if (!out) return -1;
    if (g_lib.state != WF_LIB_STATE_MOUNTED) return -1;
    if (string_offset == WFLIB_STRING_NONE) return 0;

    uint32_t bucket_idx = WFLIB_STRING_BUCKET(string_offset);
    uint32_t intra_idx  = WFLIB_STRING_INTRA(string_offset);
    if (bucket_count == 0) return -1;
    if (bucket_idx >= bucket_count) return -1;
    if (intra_idx >= 16) return -1;

    /* Read the 4-byte bucket pointer from the bucket table. */
    uint8_t ptr_buf[4];
    off_t tbl_off = (off_t)bucket_table_abs + (off_t)bucket_idx * 4;
    if (read_at(tbl_off, ptr_buf, 4) != 0) return -1;
    uint32_t bucket_abs = rd_u32(ptr_buf);
    /* Bucket must lie inside the file. We don't know which pool's section
     * bounds to check without an extra arg, so we use file_length as the
     * outer bound. The bucket header's bucket_byte_len is re-checked
     * against the scratch buffer below. */
    if (bucket_abs < 64 || bucket_abs >= g_lib.hdr.file_length) return -1;
    if ((uint64_t)bucket_abs + 4 > g_lib.hdr.file_length) return -1;

    /* Pin scratch, read first 2 bytes to learn bucket_byte_len, then
     * read the rest. Hold the pin across both reads — pin is cheap and
     * the alternative (re-pin) re-validates the handle for no gain. */
    void *scratch = core_get_data_pinned(g_lib.scratch_pool_handle);
    int rc = -1;

    if (read_at((off_t)bucket_abs, scratch, 2) != 0) goto out;
    uint16_t bucket_byte_len = rd_u16((const uint8_t *)scratch);
    if (bucket_byte_len < 4 || bucket_byte_len > WFLIB_SCRATCH_BYTES) goto out;
    if ((uint64_t)bucket_abs + bucket_byte_len > g_lib.hdr.file_length) goto out;
    /* Read the remaining (bucket_byte_len - 2) bytes into scratch+2. */
    if (read_at((off_t)bucket_abs + 2, (uint8_t *)scratch + 2,
                bucket_byte_len - 2) != 0) goto out;

    rc = decode_bucket_string((const uint8_t *)scratch, bucket_byte_len,
                              intra_idx, out, buflen);

out:
    core_put_data_pinned(scratch);
    return rc;
}

int wf_lib_resolve_string(uint32_t off, char *out, size_t buflen) {
    mutex_lock(&g_lib_mutex);
    int rc = resolve_pool(off, out, buflen,
                          g_lib.string_pool_bucket_count,
                          g_lib.string_pool_bucket_table_abs);
    mutex_unlock(&g_lib_mutex);
    return rc;
}
int wf_lib_resolve_path(uint32_t off, char *out, size_t buflen) {
    mutex_lock(&g_lib_mutex);
    int rc = resolve_pool(off, out, buflen,
                          g_lib.path_pool_bucket_count,
                          g_lib.path_pool_bucket_table_abs);
    mutex_unlock(&g_lib_mutex);
    return rc;
}

#endif /* WAVEFORM_WFLIB */
