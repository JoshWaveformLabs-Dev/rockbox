/* Waveform m4: stub implementations for tagcache/tagtree when HAVE_TAGCACHE is disabled */
#include "config.h"
#ifndef HAVE_TAGCACHE

#include <stdbool.h>
#include <stddef.h>
#include "tagcache.h"
#include "tagtree.h"

/* tagcache stubs */
void tagcache_init(void) {}
bool tagcache_is_initialized(void) { return false; }
bool tagcache_is_usable(void) { return false; }
void tagcache_start_scan(void) {}
bool tagcache_update(void) { return false; }
bool tagcache_rebuild(void) { return false; }
int  tagcache_get_commit_step(void) { return 0; }
int  tagcache_get_max_commit_step(void) { return 0; }
bool tagcache_prepare_shutdown(void) { return true; }
void tagcache_shutdown(void) {}
void tagcache_remove_statefile(void) {}
void tagcache_unload_ramcache(void) {}
struct tagcache_stat* tagcache_get_stat(void) { return NULL; }
void tagcache_screensync_event(void) {}
void tagcache_screensync_enable(bool state) { (void)state; }

/* tagtree stubs */
void tagtree_init(void) {}
int  tagtree_export(void) { return -1; }
int  tagtree_import(void) { return -1; }
int  tagtree_enter(struct tree_context *c, bool is_visible)
    { (void)c; (void)is_visible; return -1; }
void tagtree_exit(struct tree_context *c, bool is_visible)
    { (void)c; (void)is_visible; }
int  tagtree_load(struct tree_context *c) { (void)c; return -1; }
char *tagtree_get_entry_name(struct tree_context *c, int id, char *buf, size_t bufsize)
    { (void)c; (void)id; (void)bufsize; if (buf) buf[0] = '\0'; return buf; }
bool tagtree_current_playlist_insert(int position, bool queue)
    { (void)position; (void)queue; return false; }
char *tagtree_get_title(struct tree_context *c) { (void)c; return NULL; }
int  tagtree_get_attr(struct tree_context *c) { (void)c; return 0; }
int  tagtree_get_icon(struct tree_context *c) { (void)c; return 0; }
int  tagtree_get_custom_action(struct tree_context *c) { (void)c; return -1; }
int  tagtree_add_to_playlist(const char *playlist, bool new_playlist)
    { (void)playlist; (void)new_playlist; return -1; }
bool tagtree_get_subentry_filename(char *buf, size_t bufsize)
    { (void)bufsize; if (buf) buf[0] = '\0'; return false; }
bool tagtree_subentries_do_action(bool (*action_cb)(const char *file_name))
    { (void)action_cb; return false; }

/* tagcache search API stubs */
bool tagcache_search(struct tagcache_search *tcs, int tag)
    { (void)tcs; (void)tag; return false; }
void tagcache_search_set_uniqbuf(struct tagcache_search *tcs, void *buf, long size)
    { (void)tcs; (void)buf; (void)size; }
bool tagcache_search_add_filter(struct tagcache_search *tcs, int tag, int seek)
    { (void)tcs; (void)tag; (void)seek; return false; }
bool tagcache_get_next(struct tagcache_search *tcs, char *buf, long size)
    { (void)tcs; (void)buf; (void)size; return false; }
bool tagcache_retrieve(struct tagcache_search *tcs, int idxid, int tag, char *buf, long size)
    { (void)tcs; (void)idxid; (void)tag; (void)buf; (void)size; return false; }
void tagcache_search_finish(struct tagcache_search *tcs) { (void)tcs; }
long tagcache_get_numeric(const struct tagcache_search *tcs, int tag)
    { (void)tcs; (void)tag; return 0; }
void tagcache_update_numeric(int idx_id, int tag, long data)
    { (void)idx_id; (void)tag; (void)data; }
void tagcache_commit_finalize(void) {}

#endif /* !HAVE_TAGCACHE */
