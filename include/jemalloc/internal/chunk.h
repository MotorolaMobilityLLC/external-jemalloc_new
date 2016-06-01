/******************************************************************************/
#ifdef JEMALLOC_H_TYPES

/*
 * Size and alignment of memory chunks that are allocated by the OS's virtual
 * memory system.
 */
#define	LG_CHUNK_DEFAULT	21

/* Return the smallest chunk multiple that is >= s. */
#define	CHUNK_CEILING(s)						\
	(((s) + chunksize_mask) & ~chunksize_mask)

#endif /* JEMALLOC_H_TYPES */
/******************************************************************************/
#ifdef JEMALLOC_H_STRUCTS

#endif /* JEMALLOC_H_STRUCTS */
/******************************************************************************/
#ifdef JEMALLOC_H_EXTERNS

extern size_t		opt_lg_chunk;
extern const char	*opt_dss;

extern size_t		chunksize;
extern size_t		chunksize_mask; /* (chunksize - 1). */
extern size_t		chunk_npages;

extern const extent_hooks_t	extent_hooks_default;

extent_hooks_t	extent_hooks_get(tsdn_t *tsdn, arena_t *arena);
extent_hooks_t	extent_hooks_set(tsdn_t *tsdn, arena_t *arena,
    const extent_hooks_t *extent_hooks);

extent_t	*chunk_alloc_cache(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool slab);
extent_t	*chunk_alloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, void *new_addr, size_t usize, size_t pad,
    size_t alignment, bool *zero, bool *commit, bool slab);
void	chunk_dalloc_cache(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *extent);
void	chunk_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *extent);
bool	chunk_commit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *extent, size_t offset,
    size_t length);
bool	chunk_decommit_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *extent, size_t offset,
    size_t length);
bool	chunk_purge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *extent, size_t offset,
    size_t length);
extent_t	*chunk_split_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *extent, size_t size_a,
    size_t usize_a, size_t size_b, size_t usize_b);
bool	chunk_merge_wrapper(tsdn_t *tsdn, arena_t *arena,
    extent_hooks_t *extent_hooks, extent_t *a, extent_t *b);
bool	chunk_boot(void);
void	chunk_prefork(tsdn_t *tsdn);
void	chunk_postfork_parent(tsdn_t *tsdn);
void	chunk_postfork_child(tsdn_t *tsdn);

#endif /* JEMALLOC_H_EXTERNS */
/******************************************************************************/
#ifdef JEMALLOC_H_INLINES

#endif /* JEMALLOC_H_INLINES */
/******************************************************************************/

#include "jemalloc/internal/chunk_dss.h"
#include "jemalloc/internal/chunk_mmap.h"
