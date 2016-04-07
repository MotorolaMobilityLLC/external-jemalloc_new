#define	JEMALLOC_CHUNK_C_
#include "jemalloc/internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

const char	*opt_dss = DSS_DEFAULT;
size_t		opt_lg_chunk = 0;

/* Used exclusively for gdump triggering. */
static size_t	curchunks;
static size_t	highchunks;

rtree_t		chunks_rtree;

/* Various chunk-related settings. */
size_t		chunksize;
size_t		chunksize_mask; /* (chunksize - 1). */
size_t		chunk_npages;

static void	*chunk_alloc_default(void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, unsigned arena_ind);
static bool	chunk_dalloc_default(void *chunk, size_t size, bool committed,
    unsigned arena_ind);
static bool	chunk_commit_default(void *chunk, size_t size, size_t offset,
    size_t length, unsigned arena_ind);
static bool	chunk_decommit_default(void *chunk, size_t size, size_t offset,
    size_t length, unsigned arena_ind);
static bool	chunk_purge_default(void *chunk, size_t size, size_t offset,
    size_t length, unsigned arena_ind);
static bool	chunk_split_default(void *chunk, size_t size, size_t size_a,
    size_t size_b, bool committed, unsigned arena_ind);
static bool	chunk_merge_default(void *chunk_a, size_t size_a, void *chunk_b,
    size_t size_b, bool committed, unsigned arena_ind);

const chunk_hooks_t	chunk_hooks_default = {
	chunk_alloc_default,
	chunk_dalloc_default,
	chunk_commit_default,
	chunk_decommit_default,
	chunk_purge_default,
	chunk_split_default,
	chunk_merge_default
};

/******************************************************************************/
/*
 * Function prototypes for static functions that are referenced prior to
 * definition.
 */

static void	chunk_record(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, extent_tree_t *chunks_szad,
    extent_tree_t *chunks_ad, bool cache, void *chunk, size_t size, bool zeroed,
    bool committed);

/******************************************************************************/

static chunk_hooks_t
chunk_hooks_get_locked(arena_t *arena)
{

	return (arena->chunk_hooks);
}

chunk_hooks_t
chunk_hooks_get(tsdn_t *tsdn, arena_t *arena)
{
	chunk_hooks_t chunk_hooks;

	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	chunk_hooks = chunk_hooks_get_locked(arena);
	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);

	return (chunk_hooks);
}

chunk_hooks_t
chunk_hooks_set(tsdn_t *tsdn, arena_t *arena, const chunk_hooks_t *chunk_hooks)
{
	chunk_hooks_t old_chunk_hooks;

	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	old_chunk_hooks = arena->chunk_hooks;
	/*
	 * Copy each field atomically so that it is impossible for readers to
	 * see partially updated pointers.  There are places where readers only
	 * need one hook function pointer (therefore no need to copy the
	 * entirety of arena->chunk_hooks), and stale reads do not affect
	 * correctness, so they perform unlocked reads.
	 */
#define	ATOMIC_COPY_HOOK(n) do {					\
	union {								\
		chunk_##n##_t	**n;					\
		void		**v;					\
	} u;								\
	u.n = &arena->chunk_hooks.n;					\
	atomic_write_p(u.v, chunk_hooks->n);				\
} while (0)
	ATOMIC_COPY_HOOK(alloc);
	ATOMIC_COPY_HOOK(dalloc);
	ATOMIC_COPY_HOOK(commit);
	ATOMIC_COPY_HOOK(decommit);
	ATOMIC_COPY_HOOK(purge);
	ATOMIC_COPY_HOOK(split);
	ATOMIC_COPY_HOOK(merge);
#undef ATOMIC_COPY_HOOK
	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);

	return (old_chunk_hooks);
}

static void
chunk_hooks_assure_initialized_impl(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks, bool locked)
{
	static const chunk_hooks_t uninitialized_hooks =
	    CHUNK_HOOKS_INITIALIZER;

	if (memcmp(chunk_hooks, &uninitialized_hooks, sizeof(chunk_hooks_t)) ==
	    0) {
		*chunk_hooks = locked ? chunk_hooks_get_locked(arena) :
		    chunk_hooks_get(tsdn, arena);
	}
}

static void
chunk_hooks_assure_initialized_locked(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks)
{

	chunk_hooks_assure_initialized_impl(tsdn, arena, chunk_hooks, true);
}

static void
chunk_hooks_assure_initialized(tsdn_t *tsdn, arena_t *arena,
    chunk_hooks_t *chunk_hooks)
{

	chunk_hooks_assure_initialized_impl(tsdn, arena, chunk_hooks, false);
}

bool
chunk_register(tsdn_t *tsdn, const void *chunk, const extent_t *extent)
{
	size_t size;
	rtree_elm_t *elm_a;

	assert(extent_addr_get(extent) == chunk);

	size = extent_size_get(extent);

	if ((elm_a = rtree_elm_acquire(tsdn, &chunks_rtree, (uintptr_t)chunk,
	    false, true)) == NULL)
		return (true);
	rtree_elm_write_acquired(tsdn, &chunks_rtree, elm_a, extent);
	if (size > chunksize) {
		uintptr_t last = ((uintptr_t)chunk +
		    (uintptr_t)(CHUNK_CEILING(size - chunksize)));
		rtree_elm_t *elm_b;

		if ((elm_b = rtree_elm_acquire(tsdn, &chunks_rtree, last, false,
		    true)) == NULL) {
			rtree_elm_write_acquired(tsdn, &chunks_rtree, elm_a,
			    NULL);
			rtree_elm_release(tsdn, &chunks_rtree, elm_a);
			return (true);
		}
		rtree_elm_write_acquired(tsdn, &chunks_rtree, elm_b, extent);
		rtree_elm_release(tsdn, &chunks_rtree, elm_b);
	}
	rtree_elm_release(tsdn, &chunks_rtree, elm_a);

	if (config_prof && opt_prof) {
		size_t nadd = (size == 0) ? 1 : size / chunksize;
		size_t cur = atomic_add_z(&curchunks, nadd);
		size_t high = atomic_read_z(&highchunks);
		while (cur > high && atomic_cas_z(&highchunks, high, cur)) {
			/*
			 * Don't refresh cur, because it may have decreased
			 * since this thread lost the highchunks update race.
			 */
			high = atomic_read_z(&highchunks);
		}
		if (cur > high && prof_gdump_get_unlocked())
			prof_gdump(tsdn);
	}

	return (false);
}

void
chunk_deregister(tsdn_t *tsdn, const void *chunk, const extent_t *extent)
{
	size_t size;
	rtree_elm_t *elm_a;

	size = extent_size_get(extent);

	elm_a = rtree_elm_acquire(tsdn, &chunks_rtree, (uintptr_t)chunk, true,
	    false);
	rtree_elm_write_acquired(tsdn, &chunks_rtree, elm_a, NULL);
	if (size > chunksize) {
		uintptr_t last = ((uintptr_t)chunk +
		    (uintptr_t)(CHUNK_CEILING(size - chunksize)));
		rtree_elm_t *elm_b = rtree_elm_acquire(tsdn, &chunks_rtree,
		    last, true, false);

		rtree_elm_write_acquired(tsdn, &chunks_rtree, elm_b, NULL);
		rtree_elm_release(tsdn, &chunks_rtree, elm_b);
	}
	rtree_elm_release(tsdn, &chunks_rtree, elm_a);

	if (config_prof && opt_prof) {
		size_t nsub = (size == 0) ? 1 : size / chunksize;
		assert(atomic_read_z(&curchunks) >= nsub);
		atomic_sub_z(&curchunks, nsub);
	}
}

void
chunk_reregister(tsdn_t *tsdn, const void *chunk, const extent_t *extent)
{
	bool err;

	err = chunk_register(tsdn, chunk, extent);
	assert(!err);
}

/*
 * Do first-best-fit chunk selection, i.e. select the lowest chunk that best
 * fits.
 */
static extent_t *
chunk_first_best_fit(arena_t *arena, extent_tree_t *chunks_szad,
    extent_tree_t *chunks_ad, size_t size)
{
	extent_t key;

	assert(size == CHUNK_CEILING(size));

	extent_init(&key, arena, NULL, size, false, false, false, false);
	return (extent_tree_szad_nsearch(chunks_szad, &key));
}

static void *
chunk_recycle(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    extent_tree_t *chunks_szad, extent_tree_t *chunks_ad, bool cache,
    void *new_addr, size_t size, size_t alignment, bool *zero, bool *commit,
    bool dalloc_extent)
{
	void *ret;
	extent_t *extent;
	size_t alloc_size, leadsize, trailsize;
	bool zeroed, committed;

	assert(new_addr == NULL || alignment == chunksize);
	/*
	 * Cached chunks use the extent linkage embedded in their headers, in
	 * which case dalloc_extent is true, and new_addr is non-NULL because
	 * we're operating on a specific chunk.
	 */
	assert(dalloc_extent || new_addr != NULL);

	alloc_size = CHUNK_CEILING(s2u(size + alignment - chunksize));
	/* Beware size_t wrap-around. */
	if (alloc_size < size)
		return (NULL);
	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	chunk_hooks_assure_initialized_locked(tsdn, arena, chunk_hooks);
	if (new_addr != NULL) {
		extent_t key;
		extent_init(&key, arena, new_addr, alloc_size, false, false,
		    false, false);
		extent = extent_tree_ad_search(chunks_ad, &key);
	} else {
		extent = chunk_first_best_fit(arena, chunks_szad, chunks_ad,
		    alloc_size);
	}
	if (extent == NULL || (new_addr != NULL && extent_size_get(extent) <
	    size)) {
		malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
		return (NULL);
	}
	leadsize = ALIGNMENT_CEILING((uintptr_t)extent_addr_get(extent),
	    alignment) - (uintptr_t)extent_addr_get(extent);
	assert(new_addr == NULL || leadsize == 0);
	assert(extent_size_get(extent) >= leadsize + size);
	trailsize = extent_size_get(extent) - leadsize - size;
	ret = (void *)((uintptr_t)extent_addr_get(extent) + leadsize);
	zeroed = extent_zeroed_get(extent);
	if (zeroed)
		*zero = true;
	committed = extent_committed_get(extent);
	if (committed)
		*commit = true;
	/* Split the lead. */
	if (leadsize != 0 &&
	    chunk_hooks->split(extent_addr_get(extent),
	    extent_size_get(extent), leadsize, size, false, arena->ind)) {
		malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
		return (NULL);
	}
	/* Remove extent from the tree. */
	extent_tree_szad_remove(chunks_szad, extent);
	extent_tree_ad_remove(chunks_ad, extent);
	arena_chunk_cache_maybe_remove(arena, extent, cache);
	if (leadsize != 0) {
		/* Insert the leading space as a smaller chunk. */
		extent_size_set(extent, leadsize);
		extent_tree_szad_insert(chunks_szad, extent);
		extent_tree_ad_insert(chunks_ad, extent);
		arena_chunk_cache_maybe_insert(arena, extent, cache);
		extent = NULL;
	}
	if (trailsize != 0) {
		/* Split the trail. */
		if (chunk_hooks->split(ret, size + trailsize, size,
		    trailsize, false, arena->ind)) {
			if (dalloc_extent && extent != NULL)
				arena_extent_dalloc(tsdn, arena, extent);
			malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
			chunk_record(tsdn, arena, chunk_hooks, chunks_szad,
			    chunks_ad, cache, ret, size + trailsize, zeroed,
			    committed);
			return (NULL);
		}
		/* Insert the trailing space as a smaller chunk. */
		if (extent == NULL) {
			extent = arena_extent_alloc(tsdn, arena);
			if (extent == NULL) {
				malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
				chunk_record(tsdn, arena, chunk_hooks,
				    chunks_szad, chunks_ad, cache, ret, size +
				    trailsize, zeroed, committed);
				return (NULL);
			}
		}
		extent_init(extent, arena, (void *)((uintptr_t)(ret) + size),
		    trailsize, false, zeroed, committed, false);
		extent_tree_szad_insert(chunks_szad, extent);
		extent_tree_ad_insert(chunks_ad, extent);
		arena_chunk_cache_maybe_insert(arena, extent, cache);
		extent = NULL;
	}
	if (!committed && chunk_hooks->commit(ret, size, 0, size, arena->ind)) {
		malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
		chunk_record(tsdn, arena, chunk_hooks, chunks_szad, chunks_ad,
		    cache, ret, size, zeroed, committed);
		return (NULL);
	}
	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);

	assert(dalloc_extent || extent != NULL);
	if (dalloc_extent && extent != NULL)
		arena_extent_dalloc(tsdn, arena, extent);
	if (*zero) {
		if (!zeroed)
			memset(ret, 0, size);
		else if (config_debug) {
			size_t i;
			size_t *p = (size_t *)(uintptr_t)ret;

			for (i = 0; i < size / sizeof(size_t); i++)
				assert(p[i] == 0);
		}
	}
	return (ret);
}

/*
 * If the caller specifies (!*zero), it is still possible to receive zeroed
 * memory, in which case *zero is toggled to true.  arena_chunk_alloc() takes
 * advantage of this to avoid demanding zeroed chunks, but taking advantage of
 * them if they are returned.
 */
static void *
chunk_alloc_core(tsdn_t *tsdn, arena_t *arena, void *new_addr, size_t size,
    size_t alignment, bool *zero, bool *commit, dss_prec_t dss_prec)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);
	assert(alignment != 0);
	assert((alignment & chunksize_mask) == 0);

	/* "primary" dss. */
	if (have_dss && dss_prec == dss_prec_primary && (ret =
	    chunk_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL)
		return (ret);
	/* mmap. */
	if ((ret = chunk_alloc_mmap(new_addr, size, alignment, zero, commit)) !=
	    NULL)
		return (ret);
	/* "secondary" dss. */
	if (have_dss && dss_prec == dss_prec_secondary && (ret =
	    chunk_alloc_dss(tsdn, arena, new_addr, size, alignment, zero,
	    commit)) != NULL)
		return (ret);

	/* All strategies for allocation failed. */
	return (NULL);
}

void *
chunk_alloc_base(size_t size)
{
	void *ret;
	bool zero, commit;

	/*
	 * Directly call chunk_alloc_mmap() rather than chunk_alloc_core()
	 * because it's critical that chunk_alloc_base() return untouched
	 * demand-zeroed virtual memory.
	 */
	zero = true;
	commit = true;
	ret = chunk_alloc_mmap(NULL, size, chunksize, &zero, &commit);
	if (ret == NULL)
		return (NULL);

	return (ret);
}

void *
chunk_alloc_cache(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    void *new_addr, size_t size, size_t alignment, bool *zero,
    bool dalloc_extent)
{
	void *ret;
	bool commit;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);
	assert(alignment != 0);
	assert((alignment & chunksize_mask) == 0);

	commit = true;
	ret = chunk_recycle(tsdn, arena, chunk_hooks,
	    &arena->chunks_szad_cached, &arena->chunks_ad_cached, true,
	    new_addr, size, alignment, zero, &commit, dalloc_extent);
	if (ret == NULL)
		return (NULL);
	assert(commit);
	return (ret);
}

static arena_t *
chunk_arena_get(tsdn_t *tsdn, unsigned arena_ind)
{
	arena_t *arena;

	arena = arena_get(tsdn, arena_ind, false);
	/*
	 * The arena we're allocating on behalf of must have been initialized
	 * already.
	 */
	assert(arena != NULL);
	return (arena);
}

static void *
chunk_alloc_default(void *new_addr, size_t size, size_t alignment, bool *zero,
    bool *commit, unsigned arena_ind)
{
	void *ret;
	tsdn_t *tsdn;
	arena_t *arena;

	tsdn = tsdn_fetch();
	arena = chunk_arena_get(tsdn, arena_ind);
	ret = chunk_alloc_core(tsdn, arena, new_addr, size, alignment, zero,
	    commit, arena->dss_prec);
	if (ret == NULL)
		return (NULL);

	return (ret);
}

static void *
chunk_alloc_retained(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    void *new_addr, size_t size, size_t alignment, bool *zero, bool *commit)
{
	void *ret;

	assert(size != 0);
	assert((size & chunksize_mask) == 0);
	assert(alignment != 0);
	assert((alignment & chunksize_mask) == 0);

	ret = chunk_recycle(tsdn, arena, chunk_hooks,
	    &arena->chunks_szad_retained, &arena->chunks_ad_retained, false,
	    new_addr, size, alignment, zero, commit, true);

	if (config_stats && ret != NULL)
		arena->stats.retained -= size;

	return (ret);
}

void *
chunk_alloc_wrapper(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    void *new_addr, size_t size, size_t alignment, bool *zero, bool *commit)
{
	void *ret;

	chunk_hooks_assure_initialized(tsdn, arena, chunk_hooks);

	ret = chunk_alloc_retained(tsdn, arena, chunk_hooks, new_addr, size,
	    alignment, zero, commit);
	if (ret == NULL) {
		ret = chunk_hooks->alloc(new_addr, size, alignment, zero,
		    commit, arena->ind);
		if (ret == NULL)
			return (NULL);
	}

	return (ret);
}

static void
chunk_record(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    extent_tree_t *chunks_szad, extent_tree_t *chunks_ad, bool cache,
    void *chunk, size_t size, bool zeroed, bool committed)
{
	bool unzeroed;
	extent_t *extent, *prev;
	extent_t key;

	assert(!cache || !zeroed);
	unzeroed = cache || !zeroed;

	malloc_mutex_lock(tsdn, &arena->chunks_mtx);
	chunk_hooks_assure_initialized_locked(tsdn, arena, chunk_hooks);
	extent_init(&key, arena, (void *)((uintptr_t)chunk + size), 0, false,
	    false, false, false);
	extent = extent_tree_ad_nsearch(chunks_ad, &key);
	/* Try to coalesce forward. */
	if (extent != NULL && extent_addr_get(extent) == extent_addr_get(&key)
	    && extent_committed_get(extent) == committed &&
	    !chunk_hooks->merge(chunk, size, extent_addr_get(extent),
	    extent_size_get(extent), false, arena->ind)) {
		/*
		 * Coalesce chunk with the following address range.  This does
		 * not change the position within chunks_ad, so only
		 * remove/insert from/into chunks_szad.
		 */
		extent_tree_szad_remove(chunks_szad, extent);
		arena_chunk_cache_maybe_remove(arena, extent, cache);
		extent_addr_set(extent, chunk);
		extent_size_set(extent, size + extent_size_get(extent));
		extent_zeroed_set(extent, extent_zeroed_get(extent) &&
		    !unzeroed);
		extent_tree_szad_insert(chunks_szad, extent);
		arena_chunk_cache_maybe_insert(arena, extent, cache);
	} else {
		/* Coalescing forward failed, so insert a new extent. */
		extent = arena_extent_alloc(tsdn, arena);
		if (extent == NULL) {
			/*
			 * Node allocation failed, which is an exceedingly
			 * unlikely failure.  Leak chunk after making sure its
			 * pages have already been purged, so that this is only
			 * a virtual memory leak.
			 */
			if (cache) {
				chunk_purge_wrapper(tsdn, arena, chunk_hooks,
				    chunk, size, 0, size);
			}
			goto label_return;
		}
		extent_init(extent, arena, chunk, size, false, !unzeroed,
		    committed, false);
		extent_tree_ad_insert(chunks_ad, extent);
		extent_tree_szad_insert(chunks_szad, extent);
		arena_chunk_cache_maybe_insert(arena, extent, cache);
	}

	/* Try to coalesce backward. */
	prev = extent_tree_ad_prev(chunks_ad, extent);
	if (prev != NULL && (void *)((uintptr_t)extent_addr_get(prev) +
	    extent_size_get(prev)) == chunk && extent_committed_get(prev) ==
	    committed && !chunk_hooks->merge(extent_addr_get(prev),
	    extent_size_get(prev), chunk, size, false, arena->ind)) {
		/*
		 * Coalesce chunk with the previous address range.  This does
		 * not change the position within chunks_ad, so only
		 * remove/insert extent from/into chunks_szad.
		 */
		extent_tree_szad_remove(chunks_szad, prev);
		extent_tree_ad_remove(chunks_ad, prev);
		arena_chunk_cache_maybe_remove(arena, prev, cache);
		extent_tree_szad_remove(chunks_szad, extent);
		arena_chunk_cache_maybe_remove(arena, extent, cache);
		extent_addr_set(extent, extent_addr_get(prev));
		extent_size_set(extent, extent_size_get(prev) +
		    extent_size_get(extent));
		extent_zeroed_set(extent, extent_zeroed_get(prev) &&
		    extent_zeroed_get(extent));
		extent_tree_szad_insert(chunks_szad, extent);
		arena_chunk_cache_maybe_insert(arena, extent, cache);

		arena_extent_dalloc(tsdn, arena, prev);
	}

label_return:
	malloc_mutex_unlock(tsdn, &arena->chunks_mtx);
}

void
chunk_dalloc_cache(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    void *chunk, size_t size, bool committed)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

	chunk_record(tsdn, arena, chunk_hooks, &arena->chunks_szad_cached,
	    &arena->chunks_ad_cached, true, chunk, size, false, committed);
	arena_maybe_purge(tsdn, arena);
}

static bool
chunk_dalloc_default(void *chunk, size_t size, bool committed,
    unsigned arena_ind)
{

	if (!have_dss || !chunk_in_dss(tsdn_fetch(), chunk))
		return (chunk_dalloc_mmap(chunk, size));
	return (true);
}

void
chunk_dalloc_wrapper(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    void *chunk, size_t size, bool zeroed, bool committed)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert(size != 0);
	assert((size & chunksize_mask) == 0);

	chunk_hooks_assure_initialized(tsdn, arena, chunk_hooks);
	/* Try to deallocate. */
	if (!chunk_hooks->dalloc(chunk, size, committed, arena->ind))
		return;
	/* Try to decommit; purge if that fails. */
	if (committed) {
		committed = chunk_hooks->decommit(chunk, size, 0, size,
		    arena->ind);
	}
	zeroed = !committed || !chunk_hooks->purge(chunk, size, 0, size,
	    arena->ind);
	chunk_record(tsdn, arena, chunk_hooks, &arena->chunks_szad_retained,
	    &arena->chunks_ad_retained, false, chunk, size, zeroed, committed);

	if (config_stats)
		arena->stats.retained += size;
}

static bool
chunk_commit_default(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	return (pages_commit((void *)((uintptr_t)chunk + (uintptr_t)offset),
	    length));
}

static bool
chunk_decommit_default(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	return (pages_decommit((void *)((uintptr_t)chunk + (uintptr_t)offset),
	    length));
}

static bool
chunk_purge_default(void *chunk, size_t size, size_t offset, size_t length,
    unsigned arena_ind)
{

	assert(chunk != NULL);
	assert(CHUNK_ADDR2BASE(chunk) == chunk);
	assert((offset & PAGE_MASK) == 0);
	assert(length != 0);
	assert((length & PAGE_MASK) == 0);

	return (pages_purge((void *)((uintptr_t)chunk + (uintptr_t)offset),
	    length));
}

bool
chunk_purge_wrapper(tsdn_t *tsdn, arena_t *arena, chunk_hooks_t *chunk_hooks,
    void *chunk, size_t size, size_t offset, size_t length)
{

	chunk_hooks_assure_initialized(tsdn, arena, chunk_hooks);
	return (chunk_hooks->purge(chunk, size, offset, length, arena->ind));
}

static bool
chunk_split_default(void *chunk, size_t size, size_t size_a, size_t size_b,
    bool committed, unsigned arena_ind)
{

	if (!maps_coalesce)
		return (true);
	return (false);
}

static bool
chunk_merge_default(void *chunk_a, size_t size_a, void *chunk_b, size_t size_b,
    bool committed, unsigned arena_ind)
{

	if (!maps_coalesce)
		return (true);
	if (have_dss) {
		tsdn_t *tsdn = tsdn_fetch();
		if (chunk_in_dss(tsdn, chunk_a) != chunk_in_dss(tsdn, chunk_b))
			return (true);
	}

	return (false);
}

bool
chunk_boot(void)
{
#ifdef _WIN32
	SYSTEM_INFO info;
	GetSystemInfo(&info);

	/*
	 * Verify actual page size is equal to or an integral multiple of
	 * configured page size.
	 */
	if (info.dwPageSize & ((1U << LG_PAGE) - 1))
		return (true);

	/*
	 * Configure chunksize (if not set) to match granularity (usually 64K),
	 * so pages_map will always take fast path.
	 */
	if (!opt_lg_chunk) {
		opt_lg_chunk = ffs_u((unsigned)info.dwAllocationGranularity)
		    - 1;
	}
#else
	if (!opt_lg_chunk)
		opt_lg_chunk = LG_CHUNK_DEFAULT;
#endif

	/* Set variables according to the value of opt_lg_chunk. */
	chunksize = (ZU(1) << opt_lg_chunk);
	assert(chunksize >= PAGE);
	chunksize_mask = chunksize - 1;
	chunk_npages = (chunksize >> LG_PAGE);

	if (have_dss && chunk_dss_boot())
		return (true);
	if (rtree_new(&chunks_rtree, (unsigned)((ZU(1) << (LG_SIZEOF_PTR+3)) -
	    opt_lg_chunk)))
		return (true);

	return (false);
}

void
chunk_prefork(tsdn_t *tsdn)
{

	chunk_dss_prefork(tsdn);
}

void
chunk_postfork_parent(tsdn_t *tsdn)
{

	chunk_dss_postfork_parent(tsdn);
}

void
chunk_postfork_child(tsdn_t *tsdn)
{

	chunk_dss_postfork_child(tsdn);
}
