
#ifndef __CORE_ALLOC_H__
#define __CORE_ALLOC_H__
#include <string.h>
#include <stdbool.h>
#include "config.h"
#include "buflib.h"
#include "chunk_alloc.h"

/* All functions below are wrappers for functions in buflib.h, except
 * they have a predefined context.
 *
 * S4-D1 (2026-05-20): the three alloc entry points pick up a const char *tag
 * via a macro overlay that auto-injects __func__ at the caller's scope. The
 * underlying entry points are the *_tagged() variants; existing call sites
 * (~65 across firmware/ and apps/) keep their original arity because the
 * macros expand transparently. To call the tagged form directly (or to
 * suppress the macro from inside the implementation TU), define
 * CORE_ALLOC_NO_MACRO_TAG before including this header. */
void core_allocator_init(void) INIT_ATTR;
int core_alloc_tagged(const char *tag, size_t size);
int core_alloc_ex_tagged(const char *tag, size_t size,
                         struct buflib_callbacks *ops);
int core_alloc_maximum_tagged(const char *tag, size_t *size,
                              struct buflib_callbacks *ops);

#ifndef CORE_ALLOC_NO_MACRO_TAG
#define core_alloc(size)              core_alloc_tagged(__func__, (size))
#define core_alloc_ex(size, ops)      core_alloc_ex_tagged(__func__, (size), (ops))
#define core_alloc_maximum(sptr, ops) core_alloc_maximum_tagged(__func__, (sptr), (ops))
#endif

bool core_shrink(int handle, void* new_start, size_t new_size);
void core_pin(int handle);
void core_unpin(int handle);
unsigned core_pin_count(int handle);
int core_free(int handle);
size_t core_available(void);
size_t core_allocatable(void);

#ifdef BUFLIB_DEBUG_CHECK_VALID
void core_check_valid(void);
#endif

/* DO NOT ADD wrappers for buflib_buffer_out/in. They do not call
 * the move callbacks and are therefore unsafe in the core */

#ifdef BUFLIB_DEBUG_PRINT
int core_get_num_blocks(void);
bool core_print_block_at(int block_num, char* buf, size_t bufsize);

/* frees the debug test alloc created at initialization,
 * since this is the first any further alloc should force a compaction run
 * only used if debug print is active */
bool core_test_free(void);
#endif

static inline void* core_get_data(int handle)
{
    extern struct buflib_context core_ctx;
    return buflib_get_data(&core_ctx, handle);
}

static inline void* core_get_data_pinned(int handle)
{
    extern struct buflib_context core_ctx;
    return buflib_get_data_pinned(&core_ctx, handle);
}

static inline void core_put_data_pinned(void *data)
{
    extern struct buflib_context core_ctx;
    buflib_put_data_pinned(&core_ctx, data);
}

/* core context chunk_alloc */
static inline bool core_chunk_alloc_init(struct chunk_alloc_header *hdr,
                                         size_t chunk_size, size_t max_chunks)
{
    extern struct buflib_context core_ctx;
    return chunk_alloc_init(hdr, &core_ctx, chunk_size, max_chunks);
}
#endif /* __CORE_ALLOC_H__ */
