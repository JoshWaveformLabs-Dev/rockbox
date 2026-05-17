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
 * On-disk format: docs/research/WFLIB_FORMAT_v1.md. C struct layouts:
 * firmware/include/wf_lib.h. This header is the API the rest of the OS
 * calls to mount a library index and look up tracks, dictionaries, sort
 * orders, strings, and paths.
 *
 * Contract:
 *   - Process-singleton: only one wflib mounted at a time.
 *   - Mount runs full §18 validation; any failure → INVALID state and the
 *     reader behaves as if unmounted (no panics, no error UI). Higher
 *     layers fall back to the FAT browser silently.
 *   - Working-set RAM bound: ≤ 96 KiB regardless of library size. D3 ships
 *     scratch-only (~8 KiB used); LRU caching is deferred to D4.
 *   - All caches via buflib handles. Pin/unpin is scoped tightly around
 *     each read in the implementation — callers never see buflib.
 *
 * When WAVEFORM_WFLIB is undefined the reader compiles to no-ops returning
 * UNMOUNTED / failure, so callers can compile unconditionally. */

#ifndef _WF_LIB_READER_H_
#define _WF_LIB_READER_H_

#include "config.h"
#include <stdint.h>
#include <stddef.h>
#include "wf_lib.h"

enum wf_lib_state {
    WF_LIB_STATE_UNMOUNTED = 0,
    WF_LIB_STATE_MOUNTED   = 1,
    WF_LIB_STATE_INVALID   = 2  /* mount attempted, §18 validation failed */
};

#ifdef WAVEFORM_WFLIB

/* Mount the file at `path`. Idempotent for repeat calls with the same path.
 * A call with a different path while mounted will unmount and remount.
 *
 * Return values:
 *   WF_LIB_STATE_MOUNTED   — mount succeeded, lookups are live
 *   WF_LIB_STATE_INVALID   — file exists but failed validation
 *   WF_LIB_STATE_UNMOUNTED — file could not be opened (does not exist /
 *                            permission / I/O error) */
enum wf_lib_state wf_lib_mount(const char *path);

/* Unmount: free all buflib handles, close fd. Safe even if not mounted. */
void wf_lib_unmount(void);

/* Query current mount state. */
enum wf_lib_state wf_lib_state(void);

/* Header metadata accessors. Return 0 when not mounted. */
uint32_t wf_lib_track_count(void);
uint32_t wf_lib_total_duration_s(void);
uint64_t wf_lib_builder_id(void);
uint64_t wf_lib_source_mtime(void);

/* Section element counts. Return 0 when not mounted. */
uint32_t wf_lib_artist_count(void);
uint32_t wf_lib_album_count(void);
uint32_t wf_lib_genre_count(void);
uint32_t wf_lib_composer_count(void);

/* Fetch one 32-byte track record by id. Returns 0 on success, -1 on
 * out-of-range track_id, unmounted, or I/O error. */
int wf_lib_get_track(uint32_t track_id, struct wflib_track_record *out);

/* Dictionary entry fetch. Returns 0 on success, -1 on out-of-range id,
 * unmounted, or I/O error. */
int wf_lib_get_artist(uint16_t artist_id, struct wflib_artist_entry *out);
int wf_lib_get_album(uint16_t album_id, struct wflib_album_entry *out);
int wf_lib_get_genre(uint16_t genre_id, struct wflib_genre_entry *out);
int wf_lib_get_composer(uint16_t composer_id, struct wflib_composer_entry *out);

/* Sort-order navigation. sort_type is one of WFLIB_SEC_SORT_*. Returns the
 * track_id at the given sort index, or UINT32_MAX on any error (unknown
 * sort_type, index out of range, unmounted, I/O error). */
uint32_t wf_lib_sort_track_at(uint32_t sort_type, uint32_t index);

/* Element count of a sort section. May differ from track_count for
 * SORT_BY_COMPOSER_ALBUM_TRACK if the builder omitted composer_id == 0
 * tracks. Returns 0 on unknown sort_type or unmounted. */
uint32_t wf_lib_sort_count(uint32_t sort_type);

/* Resolve a 32-bit string_offset (encoded as bucket_index<<4 | intra_index
 * per spec §9) into UTF-8 bytes. Writes up to buflen bytes into out.
 *
 * Returns:
 *   > 0  : bytes written (may be == buflen if string was truncated; NO
 *          trailing NUL is added — caller must respect the return value)
 *     0  : string_offset == WFLIB_STRING_NONE (no string)
 *    -1  : unmounted, I/O error, malformed bucket, or bounds error
 *
 * Truncation respects UTF-8 character boundaries: a partial multi-byte
 * sequence at the cutoff is never emitted. */
int wf_lib_resolve_string(uint32_t string_offset, char *out, size_t buflen);
int wf_lib_resolve_path(uint32_t path_offset, char *out, size_t buflen);

#else /* !WAVEFORM_WFLIB — stubs so callers can compile unconditionally */

static inline enum wf_lib_state wf_lib_mount(const char *path)
    { (void)path; return WF_LIB_STATE_UNMOUNTED; }
static inline void wf_lib_unmount(void) { }
static inline enum wf_lib_state wf_lib_state(void)
    { return WF_LIB_STATE_UNMOUNTED; }
static inline uint32_t wf_lib_track_count(void) { return 0; }
static inline uint32_t wf_lib_total_duration_s(void) { return 0; }
static inline uint64_t wf_lib_builder_id(void) { return 0; }
static inline uint64_t wf_lib_source_mtime(void) { return 0; }
static inline uint32_t wf_lib_artist_count(void) { return 0; }
static inline uint32_t wf_lib_album_count(void) { return 0; }
static inline uint32_t wf_lib_genre_count(void) { return 0; }
static inline uint32_t wf_lib_composer_count(void) { return 0; }
static inline int wf_lib_get_track(uint32_t id, struct wflib_track_record *o)
    { (void)id; (void)o; return -1; }
static inline int wf_lib_get_artist(uint16_t id, struct wflib_artist_entry *o)
    { (void)id; (void)o; return -1; }
static inline int wf_lib_get_album(uint16_t id, struct wflib_album_entry *o)
    { (void)id; (void)o; return -1; }
static inline int wf_lib_get_genre(uint16_t id, struct wflib_genre_entry *o)
    { (void)id; (void)o; return -1; }
static inline int wf_lib_get_composer(uint16_t id, struct wflib_composer_entry *o)
    { (void)id; (void)o; return -1; }
static inline uint32_t wf_lib_sort_track_at(uint32_t t, uint32_t i)
    { (void)t; (void)i; return UINT32_MAX; }
static inline uint32_t wf_lib_sort_count(uint32_t t) { (void)t; return 0; }
static inline int wf_lib_resolve_string(uint32_t o, char *b, size_t n)
    { (void)o; (void)b; (void)n; return -1; }
static inline int wf_lib_resolve_path(uint32_t o, char *b, size_t n)
    { (void)o; (void)b; (void)n; return -1; }

#endif /* WAVEFORM_WFLIB */

#endif /* _WF_LIB_READER_H_ */
