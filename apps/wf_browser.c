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

#ifdef WAVEFORM_WFLIB

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <dir.h>
#include <file.h>
#include <kernel.h>
#include "tree.h"
#include "core_alloc.h"
#include "filetree.h"
#include "filetypes.h"
#include "playlist.h"
#include "splash.h"
#include "lang.h"
#include "settings.h"
#include "misc.h"
#include "audio.h"
#include "root_menu.h"
#include "string-extra.h"
#include "wf_browser.h"
#include "wf_lib_reader.h"
#include "wf_lib.h"

#define WF_VPATH        "/.wflib"
#define WF_VPATH_LEN    7
#define WF_NAME_BUFLEN  256

enum wf_level {
    L_INVALID,
    L_ROOT,                  /* /.wflib                       */
    L_ARTISTS_LIST,          /* /.wflib/artists               */
    L_ARTIST_ALBUMS,         /* /.wflib/artists/<aid>         */
    L_ALBUM_TRACKS_OF_ART,   /* /.wflib/artists/<aid>/<bid>   */
    L_ALBUMS_LIST,           /* /.wflib/albums                */
    L_ALBUM_TRACKS           /* /.wflib/albums/<bid>          */
};

struct wf_path_info {
    enum wf_level level;
    uint16_t artist_id;
    uint16_t album_id;
};

static const char *skip_segment(const char *p, const char *literal, size_t litlen)
{
    if (strncmp(p, literal, litlen) != 0) return NULL;
    char after = p[litlen];
    if (after != '\0' && after != '/') return NULL;
    return p + litlen;
}

static const char *parse_uint(const char *p, uint16_t *out)
{
    if (*p < '0' || *p > '9') return NULL;
    unsigned long v = strtoul(p, (char **)&p, 10);
    if (v > 0xFFFFu) return NULL;
    *out = (uint16_t)v;
    return p;
}

static void parse_path(const char *currdir, struct wf_path_info *out)
{
    out->level = L_INVALID;
    out->artist_id = 0;
    out->album_id = 0;

    const char *p = strstr(currdir, WF_VPATH);
    if (!p) return;
    p += WF_VPATH_LEN;

    if (*p == '\0' || (*p == '/' && p[1] == '\0')) {
        out->level = L_ROOT;
        return;
    }
    if (*p != '/') return;
    p++;

    const char *q = skip_segment(p, "artists", 7);
    if (q) {
        p = q;
        if (*p == '\0') { out->level = L_ARTISTS_LIST; return; }
        if (*p != '/') return;
        p++;
        if (*p == '\0') { out->level = L_ARTISTS_LIST; return; }
        p = parse_uint(p, &out->artist_id);
        if (!p) return;
        if (*p == '\0') { out->level = L_ARTIST_ALBUMS; return; }
        if (*p != '/') return;
        p++;
        if (*p == '\0') { out->level = L_ARTIST_ALBUMS; return; }
        p = parse_uint(p, &out->album_id);
        if (!p) return;
        if (*p == '\0' || (*p == '/' && p[1] == '\0')) {
            out->level = L_ALBUM_TRACKS_OF_ART;
        }
        return;
    }

    q = skip_segment(p, "albums", 6);
    if (q) {
        p = q;
        if (*p == '\0') { out->level = L_ALBUMS_LIST; return; }
        if (*p != '/') return;
        p++;
        if (*p == '\0') { out->level = L_ALBUMS_LIST; return; }
        p = parse_uint(p, &out->album_id);
        if (!p) return;
        if (*p == '\0' || (*p == '/' && p[1] == '\0')) {
            out->level = L_ALBUM_TRACKS;
        }
        return;
    }
}

bool wf_browser_is_virtual_path(const char *currdir)
{
    const char *p = strstr(currdir, WF_VPATH);
    if (!p) return false;
    char next = p[WF_VPATH_LEN];
    return next == '\0' || next == '/';
}

/* Emit one entry into the locked tree cache. Returns 0 ok, -1 dirfull. */
static int emit_entry(struct tree_context *c, int *fid, int *name_used,
                      const char *name, int attr, uint32_t time_write_val)
{
    struct entry *dptr = tree_get_entry_at(c, *fid);
    if (!dptr) { c->dirfull = true; return -1; }
    int len = (int)strlen(name);
    if (len > c->cache.name_buffer_size - *name_used - 1) {
        c->dirfull = true;
        return -1;
    }
    char *namebuf = core_get_data(c->cache.name_buffer_handle);
    dptr->name = namebuf + *name_used;
    dptr->attr = attr;
    dptr->time_write = time_write_val;
    strcpy(dptr->name, name);
    *name_used += len + 1;
    (*fid)++;
    if (attr & ATTR_DIRECTORY) c->dirsindir++;
    return 0;
}

static void resolve_name(uint32_t string_offset, char *out, size_t outsz,
                         const char *fallback)
{
    int n = wf_lib_resolve_string(string_offset, out, outsz - 1);
    if (n <= 0) {
        strmemccpy(out, fallback, outsz);
        return;
    }
    out[n] = '\0';
}

static int load_root(struct tree_context *c, int *fid, int *name_used)
{
    if (emit_entry(c, fid, name_used,
                   (const char *)str(LANG_ARTISTS), ATTR_DIRECTORY, 0) < 0)
        return 0;
    if (emit_entry(c, fid, name_used,
                   (const char *)str(LANG_ALBUMS), ATTR_DIRECTORY, 0) < 0)
        return 0;
    return 0;
}

static int load_artists_list(struct tree_context *c, int *fid, int *name_used)
{
    uint32_t n = wf_lib_artist_count();
    char buf[WF_NAME_BUFLEN];
    for (uint32_t i = 0; i < n; i++) {
        struct wflib_artist_entry ae;
        if (wf_lib_get_artist((uint16_t)i, &ae) < 0) continue;
        resolve_name(ae.name_string_offset, buf, sizeof(buf), "<unknown>");
        if (emit_entry(c, fid, name_used, buf, ATTR_DIRECTORY, 0) < 0)
            return 0;
    }
    return 0;
}

static int load_albums_list(struct tree_context *c, int *fid, int *name_used)
{
    uint32_t n = wf_lib_album_count();
    char buf[WF_NAME_BUFLEN];
    for (uint32_t i = 0; i < n; i++) {
        struct wflib_album_entry be;
        if (wf_lib_get_album((uint16_t)i, &be) < 0) continue;
        resolve_name(be.name_string_offset, buf, sizeof(buf), "<unknown>");
        if (emit_entry(c, fid, name_used, buf, ATTR_DIRECTORY, 0) < 0)
            return 0;
    }
    return 0;
}

static int load_artist_albums(struct tree_context *c, int *fid, int *name_used,
                              uint16_t artist_id)
{
    /* Linear scan SORT_ARTIST_ALBUM with contiguous-run grouping. Acceptable
     * for D3 fixture (8 tracks); HDD-scale shortcut deferred to D4.x. */
    uint32_t n = wf_lib_sort_count(WFLIB_SEC_SORT_ARTIST_ALBUM);
    bool in_run = false;
    uint16_t last_album = 0;
    char buf[WF_NAME_BUFLEN];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tid = wf_lib_sort_track_at(WFLIB_SEC_SORT_ARTIST_ALBUM, i);
        if (tid == UINT32_MAX) break;
        struct wflib_track_record tr;
        if (wf_lib_get_track(tid, &tr) < 0) continue;
        if (tr.artist_id != artist_id) {
            if (in_run) break;
            continue;
        }
        if (!in_run || tr.album_id != last_album) {
            in_run = true;
            last_album = tr.album_id;
            struct wflib_album_entry be;
            if (wf_lib_get_album(tr.album_id, &be) == 0)
                resolve_name(be.name_string_offset, buf, sizeof(buf), "<unknown>");
            else
                strcpy(buf, "<unknown>");
            if (emit_entry(c, fid, name_used, buf, ATTR_DIRECTORY, 0) < 0)
                return 0;
        }
    }
    return 0;
}

static int load_album_tracks(struct tree_context *c, int *fid, int *name_used,
                             uint16_t album_id)
{
    uint32_t n = wf_lib_sort_count(WFLIB_SEC_SORT_ALBUM_TRACK);
    bool in_run = false;
    char buf[WF_NAME_BUFLEN];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tid = wf_lib_sort_track_at(WFLIB_SEC_SORT_ALBUM_TRACK, i);
        if (tid == UINT32_MAX) break;
        struct wflib_track_record tr;
        if (wf_lib_get_track(tid, &tr) < 0) continue;
        if (tr.album_id != album_id) {
            if (in_run) break;
            continue;
        }
        in_run = true;
        resolve_name(tr.title_string_offset, buf, sizeof(buf), "<unknown>");
        if (emit_entry(c, fid, name_used, buf, FILE_ATTR_AUDIO, tid) < 0)
            return 0;
    }
    return 0;
}

/* Map a category-level index back to the wflib id used in the path. */
static int chosen_id_for_index(const struct wf_path_info *pi, int index)
{
    if (index < 0) return -1;
    switch (pi->level) {
    case L_ARTISTS_LIST:
        if ((uint32_t)index >= wf_lib_artist_count()) return -1;
        return index;
    case L_ALBUMS_LIST:
        if ((uint32_t)index >= wf_lib_album_count()) return -1;
        return index;
    case L_ARTIST_ALBUMS: {
        uint32_t n = wf_lib_sort_count(WFLIB_SEC_SORT_ARTIST_ALBUM);
        int emit = 0;
        bool in_run = false;
        uint16_t last_album = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t tid = wf_lib_sort_track_at(WFLIB_SEC_SORT_ARTIST_ALBUM, i);
            if (tid == UINT32_MAX) break;
            struct wflib_track_record tr;
            if (wf_lib_get_track(tid, &tr) < 0) continue;
            if (tr.artist_id != pi->artist_id) {
                if (in_run) break;
                continue;
            }
            if (!in_run || tr.album_id != last_album) {
                if (emit == index) return tr.album_id;
                emit++;
                in_run = true;
                last_album = tr.album_id;
            }
        }
        return -1;
    }
    default:
        return -1;
    }
}

int wf_browser_load(struct tree_context *c)
{
    if (wf_lib_state() != WF_LIB_STATE_MOUNTED)
        return -1;

    struct wf_path_info pi;
    parse_path(c->currdir, &pi);
    if (pi.level == L_INVALID)
        return -1;

    c->dirsindir = 0;
    c->dirfull = false;
    int fid = 0;
    int name_used = 0;

    tree_lock_cache(c);
    switch (pi.level) {
    case L_ROOT:                load_root(c, &fid, &name_used); break;
    case L_ARTISTS_LIST:        load_artists_list(c, &fid, &name_used); break;
    case L_ALBUMS_LIST:         load_albums_list(c, &fid, &name_used); break;
    case L_ARTIST_ALBUMS:
        load_artist_albums(c, &fid, &name_used, pi.artist_id);
        break;
    case L_ALBUM_TRACKS:
    case L_ALBUM_TRACKS_OF_ART:
        load_album_tracks(c, &fid, &name_used, pi.album_id);
        break;
    default: break;
    }
    c->filesindir = fid;
    c->dirlength = fid;
    tree_unlock_cache(c);
    return 0;
}

int wf_browser_enter(struct tree_context *c)
{
    struct entry *file = tree_get_entry_at(c, c->selected_item);
    if (!file)
        return GO_TO_PREVIOUS;

    int file_attr = file->attr;
    struct wf_path_info pi;
    parse_path(c->currdir, &pi);

    if (file_attr & ATTR_DIRECTORY) {
        size_t curlen = strlen(c->currdir);
        while (curlen > 0 && c->currdir[curlen-1] == '/')
            c->currdir[--curlen] = '\0';
        char tmp[24];
        const char *seg = NULL;
        if (pi.level == L_ROOT) {
            seg = (c->selected_item == 0) ? "/artists" : "/albums";
        } else {
            int chosen = chosen_id_for_index(&pi, c->selected_item);
            if (chosen < 0) return GO_TO_PREVIOUS;
            snprintf(tmp, sizeof(tmp), "/%d", chosen);
            seg = tmp;
        }
        size_t seglen = strlen(seg);
        if (curlen + seglen + 1 > sizeof(c->currdir)) return GO_TO_PREVIOUS;
        memcpy(c->currdir + curlen, seg, seglen + 1);
        if (c->dirlevel < MAX_DIR_LEVELS)
            c->selected_item_history[c->dirlevel] = c->selected_item;
        c->dirlevel++;
        c->selected_item = 0;
        return GO_TO_PREVIOUS;
    }

    if ((file_attr & FILE_ATTR_MASK) == FILE_ATTR_AUDIO) {
        if (!warn_on_pl_erase()) return GO_TO_PREVIOUS;
        splash(0, ID2P(LANG_WAIT));
        if (playlist_create(NULL, NULL) == -1)
            return GO_TO_PREVIOUS;

        int start_index = 0;
        int audio_idx = 0;
        for (int i = 0; i < c->filesindir; i++) {
            struct entry *e = tree_get_entry_at(c, i);
            if (!e || (e->attr & FILE_ATTR_MASK) != FILE_ATTR_AUDIO) continue;
            struct wflib_track_record tr;
            if (wf_lib_get_track(e->time_write, &tr) < 0) continue;
            char path[MAX_PATH];
            int n = wf_lib_resolve_path(tr.path_offset, path, sizeof(path) - 1);
            if (n <= 0) continue;
            path[n] = '\0';
            if (i == c->selected_item) start_index = audio_idx;
            playlist_insert_track(NULL, path,
                                  PLAYLIST_INSERT_LAST, true, false);
            audio_idx++;
        }
        if (global_settings.playlist_shuffle) {
            int seed = current_tick;
            start_index = playlist_shuffle(seed, start_index);
            if (!global_settings.play_selected) start_index = 0;
        }
        playlist_start(start_index, 0, 0);
        return GO_TO_WPS;
    }

    return GO_TO_PREVIOUS;
}

#endif /* WAVEFORM_WFLIB */
