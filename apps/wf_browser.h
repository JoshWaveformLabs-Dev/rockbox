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

/* Waveform OS Stage 3 D4.3 — WFLIB virtual browser.
 *
 * Exposes a sentinel virtual path "/.wflib/" through the standard tree
 * browser. ft_load dispatches to wf_browser_load when currdir contains
 * the sentinel; ft_enter dispatches to wf_browser_enter for both
 * directory navigation (path synthesis) and audio playback (path
 * resolution via wf_lib_resolve_path -> playlist_insert_track).
 *
 * Hierarchy:
 *   /.wflib                    -> [Artists, Albums]
 *   /.wflib/artists            -> all artists
 *   /.wflib/artists/<aid>      -> albums by artist (SORT_ARTIST_ALBUM)
 *   /.wflib/artists/<aid>/<bid>-> tracks in album (SORT_ALBUM_TRACK)
 *   /.wflib/albums             -> all albums
 *   /.wflib/albums/<bid>       -> tracks in album
 *
 * When WAVEFORM_WFLIB is undefined the API compiles to no-ops returning
 * false / -1 / GO_TO_PREVIOUS so the dispatch hooks compile out cleanly. */

#ifndef _WF_BROWSER_H_
#define _WF_BROWSER_H_

#include "config.h"
#include <stdbool.h>

struct tree_context;

#ifdef WAVEFORM_WFLIB

/* True iff `currdir` starts with or descends into the "/.wflib" sentinel.
 * Boundary check: the char immediately after the sentinel must be '\0' or
 * '/'. SDL APPLICATION builds prepend root_realpath() to currdir, so we
 * use strstr rather than strncmp. */
bool wf_browser_is_virtual_path(const char *currdir);

/* Populate the tree_context cache for the level addressed by c->currdir.
 * Mirrors ft_load's contract: returns 0 on success (with c->filesindir,
 * c->dirlength, c->dirsindir populated), -1 on any failure. */
int wf_browser_load(struct tree_context *c);

/* Handle a selection. For category entries: synthesise the next virtual
 * path into c->currdir and return GO_TO_PREVIOUS (keeps browser open).
 * For audio entries: build a playlist from the cache via
 * wf_lib_resolve_path, start playback, return GO_TO_WPS. */
int wf_browser_enter(struct tree_context *c);

#else /* !WAVEFORM_WFLIB */

static inline bool wf_browser_is_virtual_path(const char *p)
    { (void)p; return false; }
static inline int wf_browser_load(struct tree_context *c)
    { (void)c; return -1; }
static inline int wf_browser_enter(struct tree_context *c)
    { (void)c; return -2 /* GO_TO_PREVIOUS */; }

#endif /* WAVEFORM_WFLIB */

#endif /* _WF_BROWSER_H_ */
