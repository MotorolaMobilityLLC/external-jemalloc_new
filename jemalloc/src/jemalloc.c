/*-
 * This allocator implementation is designed to provide scalable performance
 * for multi-threaded programs on multi-processor systems.  The following
 * features are included for this purpose:
 *
 *   + Multiple arenas are used if there are multiple CPUs, which reduces lock
 *     contention and cache sloshing.
 *
 *   + Thread-specific caching is used if there are multiple threads, which
 *     reduces the amount of locking.
 *
 *   + Cache line sharing between arenas is avoided for internal data
 *     structures.
 *
 *   + Memory is managed in chunks and runs (chunks can be split into runs),
 *     rather than as individual pages.  This provides a constant-time
 *     mechanism for associating allocations with particular arenas.
 *
 * Allocation requests are rounded up to the nearest size class, and no record
 * of the original request size is maintained.  Allocations are broken into
 * categories according to size class.  Assuming runtime defaults, 4 KiB pages
 * and a 16 byte quantum on a 32-bit system, the size classes in each category
 * are as follows:
 *
 *   |========================================|
 *   | Category | Subcategory      |     Size |
 *   |========================================|
 *   | Small    | Tiny             |        2 |
 *   |          |                  |        4 |
 *   |          |                  |        8 |
 *   |          |------------------+----------|
 *   |          | Quantum-spaced   |       16 |
 *   |          |                  |       32 |
 *   |          |                  |       48 |
 *   |          |                  |      ... |
 *   |          |                  |       96 |
 *   |          |                  |      112 |
 *   |          |                  |      128 |
 *   |          |------------------+----------|
 *   |          | Cacheline-spaced |      192 |
 *   |          |                  |      256 |
 *   |          |                  |      320 |
 *   |          |                  |      384 |
 *   |          |                  |      448 |
 *   |          |                  |      512 |
 *   |          |------------------+----------|
 *   |          | Sub-page         |      760 |
 *   |          |                  |     1024 |
 *   |          |                  |     1280 |
 *   |          |                  |      ... |
 *   |          |                  |     3328 |
 *   |          |                  |     3584 |
 *   |          |                  |     3840 |
 *   |========================================|
 *   | Medium                      |    4 KiB |
 *   |                             |    6 KiB |
 *   |                             |    8 KiB |
 *   |                             |      ... |
 *   |                             |   28 KiB |
 *   |                             |   30 KiB |
 *   |                             |   32 KiB |
 *   |========================================|
 *   | Large                       |   36 KiB |
 *   |                             |   40 KiB |
 *   |                             |   44 KiB |
 *   |                             |      ... |
 *   |                             | 1012 KiB |
 *   |                             | 1016 KiB |
 *   |                             | 1020 KiB |
 *   |========================================|
 *   | Huge                        |    1 MiB |
 *   |                             |    2 MiB |
 *   |                             |    3 MiB |
 *   |                             |      ... |
 *   |========================================|
 *
 * Different mechanisms are used accoding to category:
 *
 *   Small/medium : Each size class is segregated into its own set of runs.
 *                  Each run maintains a bitmap of which regions are
 *                  free/allocated.
 *
 *   Large : Each allocation is backed by a dedicated run.  Metadata are stored
 *           in the associated arena chunk header maps.
 *
 *   Huge : Each allocation is backed by a dedicated contiguous set of chunks.
 *          Metadata are stored in a separate red-black tree.
 *
 *******************************************************************************
 */

#define	JEMALLOC_C_
#include "internal/jemalloc_internal.h"

/******************************************************************************/
/* Data. */

malloc_mutex_t		arenas_lock;
arena_t			**arenas;
unsigned		narenas;
#ifndef NO_TLS
static unsigned		next_arena;
#endif

#ifndef NO_TLS
__thread arena_t	*arenas_map JEMALLOC_ATTR(tls_model("initial-exec"));
#endif

/* Set to true once the allocator has been initialized. */
static bool malloc_initialized = false;

/* Used to let the initializing thread recursively allocate. */
static pthread_t malloc_initializer = (unsigned long)0;

/* Used to avoid initialization races. */
static malloc_mutex_t init_lock = PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP;

#ifdef DYNAMIC_PAGE_SHIFT
size_t		pagesize;
size_t		pagesize_mask;
size_t		lg_pagesize;
#endif

unsigned	ncpus;

/* Runtime configuration options. */
const char	*JEMALLOC_P(malloc_options)
    JEMALLOC_ATTR(visibility("default"));
#ifdef JEMALLOC_DEBUG
bool	opt_abort = true;
#  ifdef JEMALLOC_FILL
bool	opt_junk = true;
#  endif
#else
bool	opt_abort = false;
#  ifdef JEMALLOC_FILL
bool	opt_junk = false;
#  endif
#endif
#ifdef JEMALLOC_SYSV
bool	opt_sysv = false;
#endif
#ifdef JEMALLOC_XMALLOC
bool	opt_xmalloc = false;
#endif
#ifdef JEMALLOC_FILL
bool	opt_zero = false;
#endif
static int	opt_narenas_lshift = 0;

/******************************************************************************/
/* Function prototypes for non-inline static functions. */

static void	wrtmessage(void *w4opaque, const char *p1, const char *p2,
    const char *p3, const char *p4);
static void	stats_print_atexit(void);
static unsigned	malloc_ncpus(void);
static bool	malloc_init_hard(void);
static void	jemalloc_prefork(void);
static void	jemalloc_postfork(void);

/******************************************************************************/
/* malloc_message() setup. */

#ifdef JEMALLOC_HAVE_ATTR
JEMALLOC_ATTR(visibility("hidden"))
#else
static
#endif
void
wrtmessage(void *w4opaque, const char *p1, const char *p2, const char *p3,
    const char *p4)
{

	if (write(STDERR_FILENO, p1, strlen(p1)) < 0
	    || write(STDERR_FILENO, p2, strlen(p2)) < 0
	    || write(STDERR_FILENO, p3, strlen(p3)) < 0
	    || write(STDERR_FILENO, p4, strlen(p4)) < 0)
		return;
}

void	(*JEMALLOC_P(malloc_message))(void *, const char *p1, const char *p2,
    const char *p3, const char *p4) JEMALLOC_ATTR(visibility("default")) =
    wrtmessage;

/******************************************************************************/
/*
 * Begin miscellaneous support functions.
 */

/* Create a new arena and insert it into the arenas array at index ind. */
arena_t *
arenas_extend(unsigned ind)
{
	arena_t *ret;

	/* Allocate enough space for trailing bins. */
	ret = (arena_t *)base_alloc(sizeof(arena_t)
	    + (sizeof(arena_bin_t) * (nbins - 1)));
	if (ret != NULL && arena_new(ret, ind) == false) {
		arenas[ind] = ret;
		return (ret);
	}
	/* Only reached if there is an OOM error. */

	/*
	 * OOM here is quite inconvenient to propagate, since dealing with it
	 * would require a check for failure in the fast path.  Instead, punt
	 * by using arenas[0].  In practice, this is an extremely unlikely
	 * failure.
	 */
	malloc_write4("<jemalloc>", ": Error initializing arena\n", "", "");
	if (opt_abort)
		abort();

	return (arenas[0]);
}

#ifndef NO_TLS
/*
 * Choose an arena based on a per-thread value (slow-path code only, called
 * only by choose_arena()).
 */
arena_t *
choose_arena_hard(void)
{
	arena_t *ret;

	if (narenas > 1) {
		malloc_mutex_lock(&arenas_lock);
		if ((ret = arenas[next_arena]) == NULL)
			ret = arenas_extend(next_arena);
		next_arena = (next_arena + 1) % narenas;
		malloc_mutex_unlock(&arenas_lock);
	} else
		ret = arenas[0];

	arenas_map = ret;

	return (ret);
}
#endif

static inline void *
ipalloc(size_t alignment, size_t size)
{
	void *ret;
	size_t ceil_size;

	/*
	 * Round size up to the nearest multiple of alignment.
	 *
	 * This done, we can take advantage of the fact that for each small
	 * size class, every object is aligned at the smallest power of two
	 * that is non-zero in the base two representation of the size.  For
	 * example:
	 *
	 *   Size |   Base 2 | Minimum alignment
	 *   -----+----------+------------------
	 *     96 |  1100000 |  32
	 *    144 | 10100000 |  32
	 *    192 | 11000000 |  64
	 *
	 * Depending on runtime settings, it is possible that arena_malloc()
	 * will further round up to a power of two, but that never causes
	 * correctness issues.
	 */
	ceil_size = (size + (alignment - 1)) & (-alignment);
	/*
	 * (ceil_size < size) protects against the combination of maximal
	 * alignment and size greater than maximal alignment.
	 */
	if (ceil_size < size) {
		/* size_t overflow. */
		return (NULL);
	}

	if (ceil_size <= PAGE_SIZE || (alignment <= PAGE_SIZE
	    && ceil_size <= arena_maxclass))
		ret = arena_malloc(ceil_size, false);
	else {
		size_t run_size;

		/*
		 * We can't achieve subpage alignment, so round up alignment
		 * permanently; it makes later calculations simpler.
		 */
		alignment = PAGE_CEILING(alignment);
		ceil_size = PAGE_CEILING(size);
		/*
		 * (ceil_size < size) protects against very large sizes within
		 * PAGE_SIZE of SIZE_T_MAX.
		 *
		 * (ceil_size + alignment < ceil_size) protects against the
		 * combination of maximal alignment and ceil_size large enough
		 * to cause overflow.  This is similar to the first overflow
		 * check above, but it needs to be repeated due to the new
		 * ceil_size value, which may now be *equal* to maximal
		 * alignment, whereas before we only detected overflow if the
		 * original size was *greater* than maximal alignment.
		 */
		if (ceil_size < size || ceil_size + alignment < ceil_size) {
			/* size_t overflow. */
			return (NULL);
		}

		/*
		 * Calculate the size of the over-size run that arena_palloc()
		 * would need to allocate in order to guarantee the alignment.
		 */
		if (ceil_size >= alignment)
			run_size = ceil_size + alignment - PAGE_SIZE;
		else {
			/*
			 * It is possible that (alignment << 1) will cause
			 * overflow, but it doesn't matter because we also
			 * subtract PAGE_SIZE, which in the case of overflow
			 * leaves us with a very large run_size.  That causes
			 * the first conditional below to fail, which means
			 * that the bogus run_size value never gets used for
			 * anything important.
			 */
			run_size = (alignment << 1) - PAGE_SIZE;
		}

		if (run_size <= arena_maxclass) {
			ret = arena_palloc(choose_arena(), alignment, ceil_size,
			    run_size);
		} else if (alignment <= chunksize)
			ret = huge_malloc(ceil_size, false);
		else
			ret = huge_palloc(alignment, ceil_size);
	}

	assert(((uintptr_t)ret & (alignment - 1)) == 0);
	return (ret);
}

static void
stats_print_atexit(void)
{

#if (defined(JEMALLOC_TCACHE) && defined(JEMALLOC_STATS))
	unsigned i;

	/*
	 * Merge stats from extant threads.  This is racy, since individual
	 * threads do not lock when recording tcache stats events.  As a
	 * consequence, the final stats may be slightly out of date by the time
	 * they are reported, if other threads continue to allocate.
	 */
	for (i = 0; i < narenas; i++) {
		arena_t *arena = arenas[i];
		if (arena != NULL) {
			tcache_t *tcache;

			malloc_mutex_lock(&arena->lock);
			ql_foreach(tcache, &arena->tcache_ql, link) {
				tcache_stats_merge(tcache, arena);
			}
			malloc_mutex_unlock(&arena->lock);
		}
	}
#endif
	JEMALLOC_P(malloc_stats_print)(NULL, NULL, NULL);
}

static inline void *
iralloc(void *ptr, size_t size)
{
	size_t oldsize;

	assert(ptr != NULL);
	assert(size != 0);

	oldsize = isalloc(ptr);

	if (size <= arena_maxclass)
		return (arena_ralloc(ptr, size, oldsize));
	else
		return (huge_ralloc(ptr, size, oldsize));
}

/*
 * End miscellaneous support functions.
 */
/******************************************************************************/
/*
 * Begin initialization functions.
 */

static unsigned
malloc_ncpus(void)
{
	unsigned ret;
	long result;

	result = sysconf(_SC_NPROCESSORS_ONLN);
	if (result == -1) {
		/* Error. */
		ret = 1;
	}
	ret = (unsigned)result;

	return (ret);
}

/*
 * FreeBSD's pthreads implementation calls malloc(3), so the malloc
 * implementation has to take pains to avoid infinite recursion during
 * initialization.
 */
static inline bool
malloc_init(void)
{

	if (malloc_initialized == false)
		return (malloc_init_hard());

	return (false);
}

static bool
malloc_init_hard(void)
{
	unsigned i;
	int linklen;
	char buf[PATH_MAX + 1];
	const char *opts;
	arena_t *init_arenas[1];

	malloc_mutex_lock(&init_lock);
	if (malloc_initialized || malloc_initializer == pthread_self()) {
		/*
		 * Another thread initialized the allocator before this one
		 * acquired init_lock, or this thread is the initializing
		 * thread, and it is recursively allocating.
		 */
		malloc_mutex_unlock(&init_lock);
		return (false);
	}
	if (malloc_initializer != (unsigned long)0) {
		/* Busy-wait until the initializing thread completes. */
		do {
			malloc_mutex_unlock(&init_lock);
			CPU_SPINWAIT;
			malloc_mutex_lock(&init_lock);
		} while (malloc_initialized == false);
		return (false);
	}

#ifdef DYNAMIC_PAGE_SHIFT
	/* Get page size. */
	{
		long result;

		result = sysconf(_SC_PAGESIZE);
		assert(result != -1);
		pagesize = (unsigned)result;

		/*
		 * We assume that pagesize is a power of 2 when calculating
		 * pagesize_mask and lg_pagesize.
		 */
		assert(((result - 1) & result) == 0);
		pagesize_mask = result - 1;
		lg_pagesize = ffs((int)result) - 1;
	}
#endif

	for (i = 0; i < 3; i++) {
		unsigned j;

		/* Get runtime configuration. */
		switch (i) {
		case 0:
			if ((linklen = readlink("/etc/jemalloc.conf", buf,
						sizeof(buf) - 1)) != -1) {
				/*
				 * Use the contents of the "/etc/jemalloc.conf"
				 * symbolic link's name.
				 */
				buf[linklen] = '\0';
				opts = buf;
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 1:
			if ((opts = getenv("JEMALLOC_OPTIONS")) != NULL) {
				/*
				 * Do nothing; opts is already initialized to
				 * the value of the JEMALLOC_OPTIONS
				 * environment variable.
				 */
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		case 2:
			if (JEMALLOC_P(malloc_options) != NULL) {
				/*
				 * Use options that were compiled into the
				 * program.
				 */
				opts = JEMALLOC_P(malloc_options);
			} else {
				/* No configuration specified. */
				buf[0] = '\0';
				opts = buf;
			}
			break;
		default:
			/* NOTREACHED */
			assert(false);
			buf[0] = '\0';
			opts = buf;
		}

		for (j = 0; opts[j] != '\0'; j++) {
			unsigned k, nreps;
			bool nseen;

			/* Parse repetition count, if any. */
			for (nreps = 0, nseen = false;; j++, nseen = true) {
				switch (opts[j]) {
					case '0': case '1': case '2': case '3':
					case '4': case '5': case '6': case '7':
					case '8': case '9':
						nreps *= 10;
						nreps += opts[j] - '0';
						break;
					default:
						goto MALLOC_OUT;
				}
			}
MALLOC_OUT:
			if (nseen == false)
				nreps = 1;

			for (k = 0; k < nreps; k++) {
				switch (opts[j]) {
				case 'a':
					opt_abort = false;
					break;
				case 'A':
					opt_abort = true;
					break;
				case 'c':
					if (opt_lg_cspace_max - 1 >
					    opt_lg_qspace_max &&
					    opt_lg_cspace_max >
					    LG_CACHELINE)
						opt_lg_cspace_max--;
					break;
				case 'C':
					if (opt_lg_cspace_max < PAGE_SHIFT
					    - 1)
						opt_lg_cspace_max++;
					break;
				case 'd':
					if (opt_lg_dirty_mult + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_dirty_mult++;
					break;
				case 'D':
					if (opt_lg_dirty_mult >= 0)
						opt_lg_dirty_mult--;
					break;
#ifdef JEMALLOC_TCACHE
				case 'g':
					if (opt_lg_tcache_gc_sweep >= 0)
						opt_lg_tcache_gc_sweep--;
					break;
				case 'G':
					if (opt_lg_tcache_gc_sweep + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_tcache_gc_sweep++;
					break;
				case 'h':
					if (opt_lg_tcache_nslots > 0)
						opt_lg_tcache_nslots--;
					break;
				case 'H':
					if (opt_lg_tcache_nslots + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_tcache_nslots++;
					break;
#endif
#ifdef JEMALLOC_FILL
				case 'j':
					opt_junk = false;
					break;
				case 'J':
					opt_junk = true;
					break;
#endif
				case 'k':
					/*
					 * Chunks always require at least one
					 * header page, plus enough room to
					 * hold a run for the largest medium
					 * size class (one page more than the
					 * size).
					 */
					if ((1U << (opt_lg_chunk - 1)) >=
					    (2U << PAGE_SHIFT) + (1U <<
					    opt_lg_medium_max))
						opt_lg_chunk--;
					break;
				case 'K':
					if (opt_lg_chunk + 1 <
					    (sizeof(size_t) << 3))
						opt_lg_chunk++;
					break;
				case 'm':
					if (opt_lg_medium_max > PAGE_SHIFT)
						opt_lg_medium_max--;
					break;
				case 'M':
					if (opt_lg_medium_max + 1 <
					    opt_lg_chunk)
						opt_lg_medium_max++;
					break;
				case 'n':
					opt_narenas_lshift--;
					break;
				case 'N':
					opt_narenas_lshift++;
					break;
#ifdef JEMALLOC_SWAP
				case 'o':
					opt_overcommit = false;
					break;
				case 'O':
					opt_overcommit = true;
					break;
#endif
				case 'p':
					opt_stats_print = false;
					break;
				case 'P':
					opt_stats_print = true;
					break;
				case 'q':
					if (opt_lg_qspace_max > LG_QUANTUM)
						opt_lg_qspace_max--;
					break;
				case 'Q':
					if (opt_lg_qspace_max + 1 <
					    opt_lg_cspace_max)
						opt_lg_qspace_max++;
					break;
#ifdef JEMALLOC_TRACE
				case 't':
					opt_trace = false;
					break;
				case 'T':
					opt_trace = true;
					break;
#endif
#ifdef JEMALLOC_SYSV
				case 'v':
					opt_sysv = false;
					break;
				case 'V':
					opt_sysv = true;
					break;
#endif
#ifdef JEMALLOC_XMALLOC
				case 'x':
					opt_xmalloc = false;
					break;
				case 'X':
					opt_xmalloc = true;
					break;
#endif
#ifdef JEMALLOC_FILL
				case 'z':
					opt_zero = false;
					break;
				case 'Z':
					opt_zero = true;
					break;
#endif
				default: {
					char cbuf[2];

					cbuf[0] = opts[j];
					cbuf[1] = '\0';
					malloc_write4("<jemalloc>",
					    ": Unsupported character "
					    "in malloc options: '", cbuf,
					    "'\n");
				}
				}
			}
		}
	}

	/* Register fork handlers. */
	if (pthread_atfork(jemalloc_prefork, jemalloc_postfork,
	    jemalloc_postfork) != 0) {
		malloc_write4("<jemalloc>", ": Error in pthread_atfork()\n", "",
		    "");
		if (opt_abort)
			abort();
	}

	if (ctl_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifdef JEMALLOC_TRACE
	if (opt_trace) {
		if (trace_boot()) {
			malloc_mutex_unlock(&init_lock);
			return (true);
		}
	}
#endif
	if (opt_stats_print) {
		/* Print statistics at exit. */
		if (atexit(stats_print_atexit) != 0) {
			malloc_write4("<jemalloc>", ": Error in atexit()\n", "",
			    "");
			if (opt_abort)
				abort();
		}
	}

	if (chunk_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (base_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	if (arena_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifdef JEMALLOC_TCACHE
	tcache_boot();
#endif

	if (huge_boot()) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

	/*
	 * Create enough scaffolding to allow recursive allocation in
	 * malloc_ncpus().
	 */
	narenas = 1;
	arenas = init_arenas;
	memset(arenas, 0, sizeof(arena_t *) * narenas);

	/*
	 * Initialize one arena here.  The rest are lazily created in
	 * choose_arena_hard().
	 */
	arenas_extend(0);
	if (arenas[0] == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}

#ifndef NO_TLS
	/*
	 * Assign the initial arena to the initial thread, in order to avoid
	 * spurious creation of an extra arena if the application switches to
	 * threaded mode.
	 */
	arenas_map = arenas[0];
#endif

	malloc_mutex_init(&arenas_lock);

	/* Get number of CPUs. */
	malloc_initializer = pthread_self();
	malloc_mutex_unlock(&init_lock);
	ncpus = malloc_ncpus();
	malloc_mutex_lock(&init_lock);

	if (ncpus > 1) {
		/*
		 * For SMP systems, create more than one arena per CPU by
		 * default.
		 */
#ifdef JEMALLOC_TCACHE
		if (tcache_nslots) {
			/*
			 * Only large object allocation/deallocation is
			 * guaranteed to acquire an arena mutex, so we can get
			 * away with fewer arenas than without thread caching.
			 */
			opt_narenas_lshift += 1;
		} else {
#endif
			/*
			 * All allocations must acquire an arena mutex, so use
			 * plenty of arenas.
			 */
			opt_narenas_lshift += 2;
#ifdef JEMALLOC_TCACHE
		}
#endif
	}

	/* Determine how many arenas to use. */
	narenas = ncpus;
	if (opt_narenas_lshift > 0) {
		if ((narenas << opt_narenas_lshift) > narenas)
			narenas <<= opt_narenas_lshift;
		/*
		 * Make sure not to exceed the limits of what base_alloc() can
		 * handle.
		 */
		if (narenas * sizeof(arena_t *) > chunksize)
			narenas = chunksize / sizeof(arena_t *);
	} else if (opt_narenas_lshift < 0) {
		if ((narenas >> -opt_narenas_lshift) < narenas)
			narenas >>= -opt_narenas_lshift;
		/* Make sure there is at least one arena. */
		if (narenas == 0)
			narenas = 1;
	}

#ifdef NO_TLS
	if (narenas > 1) {
		static const unsigned primes[] = {1, 3, 5, 7, 11, 13, 17, 19,
		    23, 29, 31, 37, 41, 43, 47, 53, 59, 61, 67, 71, 73, 79, 83,
		    89, 97, 101, 103, 107, 109, 113, 127, 131, 137, 139, 149,
		    151, 157, 163, 167, 173, 179, 181, 191, 193, 197, 199, 211,
		    223, 227, 229, 233, 239, 241, 251, 257, 263};
		unsigned nprimes, parenas;

		/*
		 * Pick a prime number of hash arenas that is more than narenas
		 * so that direct hashing of pthread_self() pointers tends to
		 * spread allocations evenly among the arenas.
		 */
		assert((narenas & 1) == 0); /* narenas must be even. */
		nprimes = (sizeof(primes) >> LG_SIZEOF_INT);
		parenas = primes[nprimes - 1]; /* In case not enough primes. */
		for (i = 1; i < nprimes; i++) {
			if (primes[i] > narenas) {
				parenas = primes[i];
				break;
			}
		}
		narenas = parenas;
	}
#endif

#ifndef NO_TLS
	next_arena = 0;
#endif

	/* Allocate and initialize arenas. */
	arenas = (arena_t **)base_alloc(sizeof(arena_t *) * narenas);
	if (arenas == NULL) {
		malloc_mutex_unlock(&init_lock);
		return (true);
	}
	/*
	 * Zero the array.  In practice, this should always be pre-zeroed,
	 * since it was just mmap()ed, but let's be sure.
	 */
	memset(arenas, 0, sizeof(arena_t *) * narenas);
	/* Copy the pointer to the one arena that was already initialized. */
	arenas[0] = init_arenas[0];

	malloc_initialized = true;
	malloc_mutex_unlock(&init_lock);
	return (false);
}

/*
 * End initialization functions.
 */
/******************************************************************************/
/*
 * Begin malloc(3)-compatible functions.
 */

JEMALLOC_ATTR(malloc)
JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(malloc)(size_t size)
{
	void *ret;

	if (malloc_init()) {
		ret = NULL;
		goto OOM;
	}

	if (size == 0) {
#ifdef JEMALLOC_SYSV
		if (opt_sysv == false)
#endif
			size = 1;
#ifdef JEMALLOC_SYSV
		else {
#  ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write4("<jemalloc>",
				    ": Error in malloc(): invalid size 0\n", "",
				    "");
				abort();
			}
#  endif
			ret = NULL;
			goto RETURN;
		}
#endif
	}

	ret = imalloc(size);

OOM:
	if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_write4("<jemalloc>",
			    ": Error in malloc(): out of memory\n", "",
			    "");
			abort();
		}
#endif
		errno = ENOMEM;
	}

#ifdef JEMALLOC_SYSV
RETURN:
#endif
#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_malloc(ret, size);
#endif
	return (ret);
}

JEMALLOC_ATTR(nonnull(1))
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(posix_memalign)(void **memptr, size_t alignment, size_t size)
{
	int ret;
	void *result;

	if (malloc_init())
		result = NULL;
	else {
		if (size == 0) {
#ifdef JEMALLOC_SYSV
			if (opt_sysv == false)
#endif
				size = 1;
#ifdef JEMALLOC_SYSV
			else {
#  ifdef JEMALLOC_XMALLOC
				if (opt_xmalloc) {
					malloc_write4("<jemalloc>",
					    ": Error in posix_memalign(): "
					    "invalid size 0\n", "", "");
					abort();
				}
#  endif
				result = NULL;
				*memptr = NULL;
				ret = 0;
				goto RETURN;
			}
#endif
		}

		/* Make sure that alignment is a large enough power of 2. */
		if (((alignment - 1) & alignment) != 0
		    || alignment < sizeof(void *)) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write4("<jemalloc>",
				    ": Error in posix_memalign(): "
				    "invalid alignment\n", "", "");
				abort();
			}
#endif
			result = NULL;
			ret = EINVAL;
			goto RETURN;
		}

		result = ipalloc(alignment, size);
	}

	if (result == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_write4("<jemalloc>",
			": Error in posix_memalign(): out of memory\n",
			"", "");
			abort();
		}
#endif
		ret = ENOMEM;
		goto RETURN;
	}

	*memptr = result;
	ret = 0;

RETURN:
#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_posix_memalign(result, alignment, size);
#endif
	return (ret);
}

JEMALLOC_ATTR(malloc)
JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(calloc)(size_t num, size_t size)
{
	void *ret;
	size_t num_size;

	if (malloc_init()) {
		num_size = 0;
		ret = NULL;
		goto RETURN;
	}

	num_size = num * size;
	if (num_size == 0) {
#ifdef JEMALLOC_SYSV
		if ((opt_sysv == false) && ((num == 0) || (size == 0)))
#endif
			num_size = 1;
#ifdef JEMALLOC_SYSV
		else {
			ret = NULL;
			goto RETURN;
		}
#endif
	/*
	 * Try to avoid division here.  We know that it isn't possible to
	 * overflow during multiplication if neither operand uses any of the
	 * most significant half of the bits in a size_t.
	 */
	} else if (((num | size) & (SIZE_T_MAX << (sizeof(size_t) << 2)))
	    && (num_size / size != num)) {
		/* size_t overflow. */
		ret = NULL;
		goto RETURN;
	}

	ret = icalloc(num_size);

RETURN:
	if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
		if (opt_xmalloc) {
			malloc_write4("<jemalloc>",
			    ": Error in calloc(): out of memory\n", "",
			    "");
			abort();
		}
#endif
		errno = ENOMEM;
	}

#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_calloc(ret, num, size);
#endif
	return (ret);
}

JEMALLOC_ATTR(visibility("default"))
void *
JEMALLOC_P(realloc)(void *ptr, size_t size)
{
	void *ret;
#ifdef JEMALLOC_TRACE
	size_t old_size;
#endif

	if (size == 0) {
#ifdef JEMALLOC_SYSV
		if (opt_sysv == false)
#endif
			size = 1;
#ifdef JEMALLOC_SYSV
		else {
			if (ptr != NULL) {
#ifdef JEMALLOC_TRACE
				if (opt_trace)
					old_size = isalloc(ptr);
#endif
				idalloc(ptr);
			}
			ret = NULL;
			goto RETURN;
		}
#endif
	}

	if (ptr != NULL) {
		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

#ifdef JEMALLOC_TRACE
		if (opt_trace)
			old_size = isalloc(ptr);
#endif

		ret = iralloc(ptr, size);

		if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write4("<jemalloc>",
				    ": Error in realloc(): out of "
				    "memory\n", "", "");
				abort();
			}
#endif
			errno = ENOMEM;
		}
	} else {
		if (malloc_init())
			ret = NULL;
		else
			ret = imalloc(size);

#ifdef JEMALLOC_TRACE
		if (opt_trace)
			old_size = 0;
#endif

		if (ret == NULL) {
#ifdef JEMALLOC_XMALLOC
			if (opt_xmalloc) {
				malloc_write4("<jemalloc>",
				    ": Error in realloc(): out of "
				    "memory\n", "", "");
				abort();
			}
#endif
			errno = ENOMEM;
		}
	}

#ifdef JEMALLOC_SYSV
RETURN:
#endif
#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_realloc(ret, ptr, size, old_size);
#endif
	return (ret);
}

JEMALLOC_ATTR(visibility("default"))
void
JEMALLOC_P(free)(void *ptr)
{

	if (ptr != NULL) {
		assert(malloc_initialized || malloc_initializer ==
		    pthread_self());

#ifdef JEMALLOC_TRACE
		if (opt_trace)
			trace_free(ptr, isalloc(ptr));
#endif
		idalloc(ptr);
	}
}

/*
 * End malloc(3)-compatible functions.
 */
/******************************************************************************/
/*
 * Begin non-standard functions.
 */

JEMALLOC_ATTR(visibility("default"))
size_t
JEMALLOC_P(malloc_usable_size)(const void *ptr)
{
	size_t ret;

	assert(ptr != NULL);
	ret = isalloc(ptr);

#ifdef JEMALLOC_TRACE
	if (opt_trace)
		trace_malloc_usable_size(ret, ptr);
#endif
	return (ret);
}

#ifdef JEMALLOC_SWAP
JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(malloc_swap_enable)(const int *fds, unsigned nfds, int prezeroed)
{

	/*
	 * Make sure malloc is initialized, because we need page size, chunk
	 * size, etc.
	 */
	if (malloc_init())
		return (-1);

	return (chunk_swap_enable(fds, nfds, (prezeroed != 0)) ? -1 : 0);
}
#endif

JEMALLOC_ATTR(visibility("default"))
void
JEMALLOC_P(malloc_stats_print)(void (*write4)(void *, const char *,
    const char *, const char *, const char *), void *w4opaque, const char *opts)
{

	stats_print(write4, w4opaque, opts);
}

JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(mallctl)(const char *name, void *oldp, size_t *oldlenp, void *newp,
    size_t newlen)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_byname(name, oldp, oldlenp, newp, newlen));
}

JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(mallctlnametomib)(const char *name, size_t *mibp, size_t *miblenp)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_nametomib(name, mibp, miblenp));
}

JEMALLOC_ATTR(visibility("default"))
int
JEMALLOC_P(mallctlbymib)(const size_t *mib, size_t miblen, void *oldp,
    size_t *oldlenp, void *newp, size_t newlen)
{

	if (malloc_init())
		return (EAGAIN);

	return (ctl_bymib(mib, miblen, oldp, oldlenp, newp, newlen));
}

/*
 * End non-standard functions.
 */
/******************************************************************************/

/*
 * The following functions are used by threading libraries for protection of
 * malloc during fork().  These functions are only called if the program is
 * running in threaded mode, so there is no need to check whether the program
 * is threaded here.
 */

static void
jemalloc_prefork(void)
{
	unsigned i;

	/* Acquire all mutexes in a safe order. */

	malloc_mutex_lock(&arenas_lock);
	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			malloc_mutex_lock(&arenas[i]->lock);
	}

	malloc_mutex_lock(&base_mtx);

	malloc_mutex_lock(&huge_mtx);

#ifdef JEMALLOC_DSS
	malloc_mutex_lock(&dss_mtx);
#endif

#ifdef JEMALLOC_SWAP
	malloc_mutex_lock(&swap_mtx);
#endif
}

static void
jemalloc_postfork(void)
{
	unsigned i;

	/* Release all mutexes, now that fork() has completed. */

#ifdef JEMALLOC_SWAP
	malloc_mutex_unlock(&swap_mtx);
#endif

#ifdef JEMALLOC_DSS
	malloc_mutex_unlock(&dss_mtx);
#endif

	malloc_mutex_unlock(&huge_mtx);

	malloc_mutex_unlock(&base_mtx);

	for (i = 0; i < narenas; i++) {
		if (arenas[i] != NULL)
			malloc_mutex_unlock(&arenas[i]->lock);
	}
	malloc_mutex_unlock(&arenas_lock);
}
