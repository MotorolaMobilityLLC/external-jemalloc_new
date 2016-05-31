#define	JEMALLOC_LARGE_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/

void *
large_malloc(tsdn_t *tsdn, arena_t *arena, size_t usize, bool zero)
{

	assert(usize == s2u(usize));

	return (large_palloc(tsdn, arena, usize, CACHELINE, zero));
}

void *
large_palloc(tsdn_t *tsdn, arena_t *arena, size_t usize, size_t alignment,
    bool zero)
{
	size_t ausize;
	extent_t *extent;
	bool is_zeroed;
	UNUSED bool idump JEMALLOC_CC_SILENCE_INIT(false);

	assert(!tsdn_null(tsdn) || arena != NULL);

	ausize = sa2u(usize, alignment);
	if (unlikely(ausize == 0 || ausize > LARGE_MAXCLASS))
		return (NULL);

	/*
	 * Copy zero into is_zeroed and pass the copy to chunk_alloc(), so that
	 * it is possible to make correct junk/zero fill decisions below.
	 */
	is_zeroed = zero;
	if (likely(!tsdn_null(tsdn)))
		arena = arena_choose(tsdn_tsd(tsdn), arena);
	if (unlikely(arena == NULL) || (extent = arena_chunk_alloc_large(tsdn,
	    arena, usize, alignment, &is_zeroed)) == NULL)
		return (NULL);

	/* Insert extent into large. */
	malloc_mutex_lock(tsdn, &arena->large_mtx);
	ql_elm_new(extent, ql_link);
	ql_tail_insert(&arena->large, extent, ql_link);
	malloc_mutex_unlock(tsdn, &arena->large_mtx);
	if (config_prof && arena_prof_accum(tsdn, arena, usize))
		prof_idump(tsdn);

	if (zero || (config_fill && unlikely(opt_zero))) {
		if (!is_zeroed) {
			memset(extent_addr_get(extent), 0,
			    extent_usize_get(extent));
		}
	} else if (config_fill && unlikely(opt_junk_alloc)) {
		memset(extent_addr_get(extent), JEMALLOC_ALLOC_JUNK,
		    extent_usize_get(extent));
	}

	arena_decay_tick(tsdn, arena);
	return (extent_addr_get(extent));
}

#ifdef JEMALLOC_JET
#undef large_dalloc_junk
#define	large_dalloc_junk JEMALLOC_N(n_large_dalloc_junk)
#endif
void
large_dalloc_junk(void *ptr, size_t usize)
{

	memset(ptr, JEMALLOC_FREE_JUNK, usize);
}
#ifdef JEMALLOC_JET
#undef large_dalloc_junk
#define	large_dalloc_junk JEMALLOC_N(large_dalloc_junk)
large_dalloc_junk_t *large_dalloc_junk = JEMALLOC_N(n_large_dalloc_junk);
#endif

static void
large_dalloc_maybe_junk(tsdn_t *tsdn, void *ptr, size_t usize)
{

	if (config_fill && have_dss && unlikely(opt_junk_free)) {
		/*
		 * Only bother junk filling if the chunk isn't about to be
		 * unmapped.
		 */
		if (!config_munmap || (have_dss && chunk_in_dss(tsdn, ptr)))
			large_dalloc_junk(ptr, usize);
			memset(ptr, JEMALLOC_FREE_JUNK, usize);
	}
}

static bool
large_ralloc_no_move_shrink(tsdn_t *tsdn, extent_t *extent, size_t usize)
{
	arena_t *arena = extent_arena_get(extent);
	size_t oldusize = extent_usize_get(extent);
	extent_hooks_t extent_hooks = extent_hooks_get(tsdn, arena);
	size_t diff = extent_size_get(extent) - (usize + large_pad);

	assert(oldusize > usize);

	/* Split excess pages. */
	if (diff != 0) {
		extent_t *trail = chunk_split_wrapper(tsdn, arena,
		    &extent_hooks, extent, usize + large_pad, usize, diff,
		    diff);
		if (trail == NULL)
			return (true);

		if (config_fill && unlikely(opt_junk_free)) {
			large_dalloc_maybe_junk(tsdn, extent_addr_get(trail),
			    extent_usize_get(trail));
		}

		arena_chunk_cache_dalloc(tsdn, arena, &extent_hooks, trail);
	}

	arena_chunk_ralloc_large_shrink(tsdn, arena, extent, oldusize);

	return (false);
}

static bool
large_ralloc_no_move_expand(tsdn_t *tsdn, extent_t *extent, size_t usize,
    bool zero)
{
	arena_t *arena = extent_arena_get(extent);
	size_t oldusize = extent_usize_get(extent);
	bool is_zeroed_trail = false;
	extent_hooks_t extent_hooks = extent_hooks_get(tsdn, arena);
	size_t trailsize = usize - extent_usize_get(extent);
	extent_t *trail;

	if ((trail = arena_chunk_cache_alloc(tsdn, arena, &extent_hooks,
	    extent_past_get(extent), trailsize, CACHELINE, &is_zeroed_trail))
	    == NULL) {
		bool commit = true;
		if ((trail = chunk_alloc_wrapper(tsdn, arena, &extent_hooks,
		    extent_past_get(extent), trailsize, 0, CACHELINE,
		    &is_zeroed_trail, &commit, false)) == NULL)
			return (true);
	}

	if (chunk_merge_wrapper(tsdn, arena, &extent_hooks, extent, trail)) {
		chunk_dalloc_wrapper(tsdn, arena, &extent_hooks, trail);
		return (true);
	}

	if (zero || (config_fill && unlikely(opt_zero))) {
		if (config_cache_oblivious) {
			/*
			 * Zero the trailing bytes of the original allocation's
			 * last page, since they are in an indeterminate state.
			 * There will always be trailing bytes, because ptr's
			 * offset from the beginning of the extent is a multiple
			 * of CACHELINE in [0 .. PAGE).
			 */
			void *zbase = (void *)
			    ((uintptr_t)extent_addr_get(extent) + oldusize);
			void *zpast = PAGE_ADDR2BASE((void *)((uintptr_t)zbase +
			    PAGE));
			size_t nzero = (uintptr_t)zpast - (uintptr_t)zbase;
			assert(nzero > 0);
			memset(zbase, 0, nzero);
		}
		if (!is_zeroed_trail) {
			memset((void *)((uintptr_t)extent_addr_get(extent) +
			    oldusize), 0, usize - oldusize);
		}
	} else if (config_fill && unlikely(opt_junk_alloc)) {
		memset((void *)((uintptr_t)extent_addr_get(extent) + oldusize),
		    JEMALLOC_ALLOC_JUNK, usize - oldusize);
	}

	arena_chunk_ralloc_large_expand(tsdn, arena, extent, oldusize);

	return (false);
}

bool
large_ralloc_no_move(tsdn_t *tsdn, extent_t *extent, size_t usize_min,
    size_t usize_max, bool zero)
{

	assert(s2u(extent_usize_get(extent)) == extent_usize_get(extent));
	/* The following should have been caught by callers. */
	assert(usize_min > 0 && usize_max <= LARGE_MAXCLASS);
	/* Both allocation sizes must be large to avoid a move. */
	assert(extent_usize_get(extent) >= LARGE_MINCLASS && usize_max >=
	    LARGE_MINCLASS);

	if (usize_max > extent_usize_get(extent)) {
		/* Attempt to expand the allocation in-place. */
		if (!large_ralloc_no_move_expand(tsdn, extent, usize_max,
		    zero)) {
			arena_decay_tick(tsdn, extent_arena_get(extent));
			return (false);
		}
		/* Try again, this time with usize_min. */
		if (usize_min < usize_max && usize_min >
		    extent_usize_get(extent) &&
		    large_ralloc_no_move_expand(tsdn, extent, usize_min,
		    zero)) {
			arena_decay_tick(tsdn, extent_arena_get(extent));
			return (false);
		}
	}

	/*
	 * Avoid moving the allocation if the existing chunk size accommodates
	 * the new size.
	 */
	if (extent_usize_get(extent) >= usize_min && extent_usize_get(extent) <=
	    usize_max) {
		arena_decay_tick(tsdn, extent_arena_get(extent));
		return (false);
	}

	/* Attempt to shrink the allocation in-place. */
	if (extent_usize_get(extent) > usize_max) {
		if (!large_ralloc_no_move_shrink(tsdn, extent, usize_max)) {
			arena_decay_tick(tsdn, extent_arena_get(extent));
			return (false);
		}
	}
	return (true);
}

static void *
large_ralloc_move_helper(tsdn_t *tsdn, arena_t *arena, size_t usize,
    size_t alignment, bool zero)
{

	if (alignment <= CACHELINE)
		return (large_malloc(tsdn, arena, usize, zero));
	return (large_palloc(tsdn, arena, usize, alignment, zero));
}

void *
large_ralloc(tsdn_t *tsdn, arena_t *arena, extent_t *extent, size_t usize,
    size_t alignment, bool zero, tcache_t *tcache)
{
	void *ret;
	size_t copysize;

	/* The following should have been caught by callers. */
	assert(usize > 0 && usize <= LARGE_MAXCLASS);
	/* Both allocation sizes must be large to avoid a move. */
	assert(extent_usize_get(extent) >= LARGE_MINCLASS && usize >=
	    LARGE_MINCLASS);

	/* Try to avoid moving the allocation. */
	if (!large_ralloc_no_move(tsdn, extent, usize, usize, zero))
		return (extent_addr_get(extent));

	/*
	 * usize and old size are different enough that we need to use a
	 * different size class.  In that case, fall back to allocating new
	 * space and copying.
	 */
	ret = large_ralloc_move_helper(tsdn, arena, usize, alignment, zero);
	if (ret == NULL)
		return (NULL);

	copysize = (usize < extent_usize_get(extent)) ? usize :
	    extent_usize_get(extent);
	memcpy(ret, extent_addr_get(extent), copysize);
	isdalloct(tsdn, extent, extent_addr_get(extent),
	    extent_usize_get(extent), tcache, true);
	return (ret);
}

static void
large_dalloc_impl(tsdn_t *tsdn, extent_t *extent, bool junked_locked)
{
	arena_t *arena;

	arena = extent_arena_get(extent);
	if (!junked_locked)
		malloc_mutex_lock(tsdn, &arena->large_mtx);
	ql_remove(&arena->large, extent, ql_link);
	if (!junked_locked) {
		malloc_mutex_unlock(tsdn, &arena->large_mtx);

		large_dalloc_maybe_junk(tsdn, extent_addr_get(extent),
		    extent_usize_get(extent));
	}
	arena_chunk_dalloc_large(tsdn, arena, extent, junked_locked);

	if (!junked_locked)
		arena_decay_tick(tsdn, arena);
}

void
large_dalloc_junked_locked(tsdn_t *tsdn, extent_t *extent)
{

	large_dalloc_impl(tsdn, extent, true);
}

void
large_dalloc(tsdn_t *tsdn, extent_t *extent)
{

	large_dalloc_impl(tsdn, extent, false);
}

size_t
large_salloc(tsdn_t *tsdn, const extent_t *extent)
{
	size_t usize;
	arena_t *arena;

	arena = extent_arena_get(extent);
	malloc_mutex_lock(tsdn, &arena->large_mtx);
	usize = extent_usize_get(extent);
	malloc_mutex_unlock(tsdn, &arena->large_mtx);

	return (usize);
}

prof_tctx_t *
large_prof_tctx_get(tsdn_t *tsdn, const extent_t *extent)
{
	prof_tctx_t *tctx;
	arena_t *arena;

	arena = extent_arena_get(extent);
	malloc_mutex_lock(tsdn, &arena->large_mtx);
	tctx = extent_prof_tctx_get(extent);
	malloc_mutex_unlock(tsdn, &arena->large_mtx);

	return (tctx);
}

void
large_prof_tctx_set(tsdn_t *tsdn, extent_t *extent, prof_tctx_t *tctx)
{
	arena_t *arena;

	arena = extent_arena_get(extent);
	malloc_mutex_lock(tsdn, &arena->large_mtx);
	extent_prof_tctx_set(extent, tctx);
	malloc_mutex_unlock(tsdn, &arena->large_mtx);
}

void
large_prof_tctx_reset(tsdn_t *tsdn, extent_t *extent)
{

	large_prof_tctx_set(tsdn, extent, (prof_tctx_t *)(uintptr_t)1U);
}
