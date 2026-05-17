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

/* Waveform OS Stage 3 compact library index.
 *
 * On-disk format: see docs/research/WFLIB_FORMAT_v1.md — this header is the
 * machine-readable mirror of that spec. The spec is authoritative; if this
 * header drifts from the spec, the spec wins.
 *
 * Format: WFLIB v1.0. Studio (desktop) writes the file; OS (this header's
 * consumers) reads it. The OS reader is a strict validator that falls back
 * silently to the FAT directory browser on any failure. Working set bound:
 * ~96 KiB regardless of library size.
 *
 * Endianness: little-endian throughout. All targets (PP5022, S5L8702, x86
 * sim host) are native LE so no swap is needed on read.
 *
 * This header defines types and constants only. The reader implementation
 * lands in a later deliverable (Stage 3 D3). Including this header is
 * harmless on any target. */

#ifndef _WF_LIB_H_
#define _WF_LIB_H_

#include <stdint.h>
#include <stddef.h>

/* ----- format version constants --------------------------------------- */

/* Magic bytes "WFLB" in file order (offset 0..3). As a little-endian
 * uint32 read at offset 0 this is 0x424C4657. The string form is the
 * canonical check (memcmp against magic[4] field). */
#define WFLIB_MAGIC_STR      "WFLB"
#define WFLIB_MAGIC_LE_U32   0x424C4657u

#define WFLIB_FORMAT_MAJOR   1
#define WFLIB_FORMAT_MINOR   0

/* The reader version this build implements. Compared against the file's
 * min_reader_{major,minor} during mount validation. */
#define WFLIB_READER_MAJOR   1
#define WFLIB_READER_MINOR   0

/* ----- section type registry ------------------------------------------ */

/* Core section IDs allocated in v1.0. Append-only — once assigned, an ID
 * is never reused for a different section. Ranges per spec §8:
 *   0x0001..0x00FF  core
 *   0x0100..0x01FF  Studio extensions (reserved)
 *   0x8000..0xFFFF  third-party / experimental */
enum {
    WFLIB_SEC_STRING_POOL                = 0x0001,
    WFLIB_SEC_PATH_POOL                  = 0x0002,
    WFLIB_SEC_DICT_ARTIST                = 0x0003,
    WFLIB_SEC_DICT_ALBUM                 = 0x0004,
    WFLIB_SEC_DICT_GENRE                 = 0x0005,
    WFLIB_SEC_DICT_COMPOSER              = 0x0006,
    WFLIB_SEC_TRACK_RECORDS              = 0x0007,
    WFLIB_SEC_SORT_TITLE                 = 0x0008,
    WFLIB_SEC_SORT_ALBUM_TRACK           = 0x0009,
    WFLIB_SEC_SORT_ARTIST_ALBUM          = 0x000A,
    WFLIB_SEC_SORT_COMPOSER_ALBUM_TRACK  = 0x000B,

    /* Reserved for v1.x additive sections — names allocated so third
     * parties don't claim them in the 0x8000+ range by accident. */
    WFLIB_SEC_MPHF_TITLE                 = 0x000C,
    WFLIB_SEC_MPHF_PATH                  = 0x000D,
    WFLIB_SEC_PLAYLIST_TABLE             = 0x000E,
    WFLIB_SEC_PLAYLIST_ENTRIES           = 0x000F,
    WFLIB_SEC_REPLAYGAIN_EXT             = 0x0010,
    WFLIB_SEC_COVER_THUMB_INDEX          = 0x0011
};

/* Section table entry flags. */
#define WFLIB_SECFLAG_REQUIRED  (1u << 0)  /* reader MUST refuse on unknown */

/* ----- codec IDs (track record codec_id field) ------------------------ */
/* MUST match the post-m9 retained codec set (DECISIONS.md 2026-05-11).
 * Adding a value requires a format_minor bump per spec §15. */
enum {
    WFLIB_CODEC_UNKNOWN  = 0,
    WFLIB_CODEC_MP3      = 1,
    WFLIB_CODEC_AAC      = 2,
    WFLIB_CODEC_FLAC     = 3,
    WFLIB_CODEC_ALAC     = 4,
    WFLIB_CODEC_OPUS     = 5
    /* 6..254 reserved; 255 sentinel */
};

/* ----- track record flags (uint8 flags field, spec §15) --------------- */
#define WFLIB_TRACK_HAS_ART             (1u << 0)
#define WFLIB_TRACK_GAPLESS_PREV        (1u << 1)
#define WFLIB_TRACK_GAPLESS_NEXT        (1u << 2)
#define WFLIB_TRACK_RG_TRACK_PRESENT    (1u << 3)
#define WFLIB_TRACK_RG_ALBUM_PRESENT    (1u << 4)
#define WFLIB_TRACK_VBR                 (1u << 5)
#define WFLIB_TRACK_COMPILATION         (1u << 6)
/* bit 7 reserved, MUST be 0 */

/* ----- sentinel values ------------------------------------------------ */

/* "no string" — used in string_offset and path_offset fields. */
#define WFLIB_STRING_NONE   0xFFFFFFFFu

/* "no ReplayGain value" — used in replaygain_*_q88 fields. INT16_MIN
 * (-32768) is chosen so any plausible RG value (typically -30..+0 dB,
 * stored as -7680..0 in q88) is unambiguously distinct. The dedicated
 * flag bits (WFLIB_TRACK_RG_*_PRESENT) are the primary mechanism;
 * INT16_MIN is the belt-and-braces check. */
#define WFLIB_RG_NONE       ((int16_t)-32768)

/* Reserved dictionary index for "Unknown / missing". Always present at
 * index 0 of every DICT section. */
#define WFLIB_DICT_UNKNOWN  0

/* ----- on-disk structures --------------------------------------------- */

/* All structures are packed with no padding. Naturally aligned at file
 * offsets per spec §4. Reader code may load these via memcpy from an
 * I/O buffer or by direct cast when buffer alignment is known to be
 * safe. Field semantics: spec §6, §7, §9–§16.
 *
 * _Static_assert lines below are load-bearing: they fail the build if a
 * struct layout drifts from the spec. */

struct __attribute__((packed)) wflib_header {
    char     magic[4];               /* "WFLB" */
    uint8_t  format_major;
    uint8_t  format_minor;
    uint8_t  min_reader_major;
    uint8_t  min_reader_minor;
    uint32_t file_length;
    uint32_t header_crc32;           /* over bytes [0..12) ∪ [16..64) */
    uint32_t section_table_offset;
    uint16_t section_table_count;
    uint16_t section_entry_size;     /* MUST be 16 in v1.x */
    uint64_t builder_id;
    uint64_t source_mtime;           /* unix epoch seconds */
    uint32_t track_count;
    uint32_t total_duration_s;
    uint8_t  reserved[16];           /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_header) == 64,
               "wflib_header layout must match spec §6 (64 bytes)");

struct __attribute__((packed)) wflib_section_entry {
    uint32_t type_id;                /* WFLIB_SEC_* */
    uint32_t offset;                 /* absolute file offset of payload */
    uint32_t length;                 /* payload bytes (excluding padding) */
    uint16_t flags;                  /* WFLIB_SECFLAG_* */
    uint16_t reserved;               /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_section_entry) == 16,
               "wflib_section_entry layout must match spec §7 (16 bytes)");

/* String pool header (at section offset, before bucket payloads). */
struct __attribute__((packed)) wflib_string_pool_header {
    uint32_t bucket_count;
    uint32_t string_count;
    uint32_t bucket_table_offset;    /* from section start */
    uint32_t reserved;               /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_string_pool_header) == 16,
               "wflib_string_pool_header layout must match spec §9");

/* Bucket header inside the string pool (variable-length payload follows). */
struct __attribute__((packed)) wflib_string_bucket_header {
    uint16_t bucket_byte_len;        /* includes header */
    uint16_t first_len;              /* bytes of string 0 (then first string) */
    /* followed by first_string[first_len], then (shared_prefix_len,
     * tail_len, tail_bytes) tuples for strings 1..15 */
};
_Static_assert(sizeof(struct wflib_string_bucket_header) == 4,
               "wflib_string_bucket_header layout must match spec §9");

/* Dictionary section header (DICT_ARTIST / DICT_ALBUM / DICT_GENRE /
 * DICT_COMPOSER all share this leading layout; payloads differ by
 * entry_size). */
struct __attribute__((packed)) wflib_dict_header {
    uint32_t entry_count;
    uint16_t entry_size;             /* 8 for artist/genre/composer; 16 for album */
    uint16_t reserved;               /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_dict_header) == 8,
               "wflib_dict_header layout must match spec §11");

struct __attribute__((packed)) wflib_artist_entry {
    uint32_t name_string_offset;
    uint16_t track_count;
    uint16_t flags;                  /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_artist_entry) == 8,
               "wflib_artist_entry layout must match spec §11 (8 bytes)");

struct __attribute__((packed)) wflib_album_entry {
    uint32_t name_string_offset;
    uint16_t artist_id;              /* album artist; 0 = various/unknown */
    uint16_t year;                   /* 0 = unknown */
    uint16_t track_count;
    uint32_t first_track_id;
    uint16_t flags;                  /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_album_entry) == 16,
               "wflib_album_entry layout must match spec §12 (16 bytes)");

struct __attribute__((packed)) wflib_genre_entry {
    uint32_t name_string_offset;
    uint16_t track_count;
    uint16_t flags;                  /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_genre_entry) == 8,
               "wflib_genre_entry layout must match spec §13 (8 bytes)");

struct __attribute__((packed)) wflib_composer_entry {
    uint32_t name_string_offset;
    uint16_t track_count;
    uint16_t flags;                  /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_composer_entry) == 8,
               "wflib_composer_entry layout must match spec §14 (8 bytes)");

/* Track records section header (records follow at section_offset + 8). */
struct __attribute__((packed)) wflib_track_records_header {
    uint32_t record_count;           /* MUST equal header.track_count */
    uint16_t record_size;            /* MUST be 32 in v1.0 */
    uint16_t reserved;               /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_track_records_header) == 8,
               "wflib_track_records_header layout must match spec §15");

/* Track record — the load-bearing 32-byte payload. Adding or removing
 * any field requires a major version bump (spec §17 rule 6). */
struct __attribute__((packed)) wflib_track_record {
    uint32_t title_string_offset;
    uint32_t path_offset;
    uint16_t artist_id;
    uint16_t album_id;
    uint16_t genre_id;
    uint16_t composer_id;
    uint16_t year;                   /* 0 = unknown */
    uint16_t duration_s;             /* caps at 65535 ≈ 18.2 h */
    uint16_t bitrate_kbps;           /* 0 = unknown */
    uint8_t  track_no;               /* 0 = unknown */
    uint8_t  disc_no;                /* 0 = single-disc or unknown */
    uint8_t  codec_id;               /* WFLIB_CODEC_* */
    uint8_t  flags;                  /* WFLIB_TRACK_* */
    int16_t  replaygain_track_q88;   /* WFLIB_RG_NONE if absent */
    int16_t  replaygain_album_q88;   /* WFLIB_RG_NONE if absent */
    uint16_t sample_rate_chz;        /* sample rate Hz / 100; 441 = 44.1 kHz */
};
_Static_assert(sizeof(struct wflib_track_record) == 32,
               "wflib_track_record layout must match spec §15 (32 bytes) — "
               "this assert is the canonical schema gate; do not bypass");

/* Sort section header (all SORT_BY_* share this; payload is a uint32
 * track_id array). */
struct __attribute__((packed)) wflib_sort_header {
    uint32_t record_count;           /* usually equals header.track_count */
    uint16_t record_size;            /* MUST be 4 in v1.0 */
    uint16_t reserved;               /* MUST be zero */
};
_Static_assert(sizeof(struct wflib_sort_header) == 8,
               "wflib_sort_header layout must match spec §16");

/* ----- helpers (compile-time only; no function declarations in v1) ---- */

/* Decode a string_offset into (bucket_index, intra_index) per spec §9. */
#define WFLIB_STRING_BUCKET(offset)  (((uint32_t)(offset)) >> 4)
#define WFLIB_STRING_INTRA(offset)   (((uint32_t)(offset)) & 0xFu)

/* Compose a string_offset from bucket + intra. */
#define WFLIB_STRING_OFFSET(bucket, intra) \
    ((((uint32_t)(bucket)) << 4) | ((uint32_t)(intra) & 0xFu))

/* Convert sample_rate_chz to Hz. Inverse: hz / 100, rounded to nearest. */
#define WFLIB_CHZ_TO_HZ(chz)   ((uint32_t)(chz) * 100u)
#define WFLIB_HZ_TO_CHZ(hz)    ((uint16_t)(((uint32_t)(hz) + 50u) / 100u))

#endif /* _WF_LIB_H_ */
