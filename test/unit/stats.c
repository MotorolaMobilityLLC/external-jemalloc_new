#include "test/jemalloc_test.h"

TEST_BEGIN(test_stats_summary)
{
	size_t sz, allocated, active, resident, mapped;
	int expected = config_stats ? 0 : ENOENT;

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.allocated", (void *)&allocated, &sz, NULL,
	    0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.active", (void *)&active, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.resident", (void *)&resident, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.mapped", (void *)&mapped, &sz, NULL, 0),
	    expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_le(allocated, active,
		    "allocated should be no larger than active");
		assert_zu_lt(active, resident,
		    "active should be less than resident");
		assert_zu_lt(active, mapped,
		    "active should be less than mapped");
	}
}
TEST_END

TEST_BEGIN(test_stats_large)
{
	void *p;
	uint64_t epoch;
	size_t allocated;
	uint64_t nmalloc, ndalloc, nrequests;
	size_t sz;
	int expected = config_stats ? 0 : ENOENT;

	p = mallocx(SMALL_MAXCLASS+1, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.large.allocated",
	    (void *)&allocated, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.large.nmalloc", (void *)&nmalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.large.ndalloc", (void *)&ndalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.large.nrequests",
	    (void *)&nrequests, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_le(nmalloc, nrequests,
		    "nmalloc should no larger than nrequests");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_summary)
{
	unsigned arena;
	void *little, *large;
	uint64_t epoch;
	size_t sz;
	int expected = config_stats ? 0 : ENOENT;
	size_t mapped;
	uint64_t npurge, nmadvise, purged;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&arena,
	    sizeof(arena)), 0, "Unexpected mallctl() failure");

	little = mallocx(SMALL_MAXCLASS, 0);
	assert_ptr_not_null(little, "Unexpected mallocx() failure");
	large = mallocx((1U << LG_LARGE_MINCLASS), 0);
	assert_ptr_not_null(large, "Unexpected mallocx() failure");

	dallocx(little, 0);
	dallocx(large, 0);

	assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    config_tcache ? 0 : ENOENT, "Unexpected mallctl() result");
	assert_d_eq(mallctl("arena.0.purge", NULL, NULL, NULL, 0), 0,
	    "Unexpected mallctl() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.mapped", (void *)&mapped, &sz, NULL,
	    0), expected, "Unexepected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.npurge", (void *)&npurge, &sz, NULL,
	    0), expected, "Unexepected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.nmadvise", (void *)&nmadvise, &sz,
	    NULL, 0), expected, "Unexepected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.purged", (void *)&purged, &sz, NULL,
	    0), expected, "Unexepected mallctl() result");

	if (config_stats) {
		assert_u64_gt(npurge, 0,
		    "At least one purge should have occurred");
		assert_u64_le(nmadvise, purged,
		    "nmadvise should be no greater than purged");
	}
}
TEST_END

void *
thd_start(void *arg)
{
	return (NULL);
}

static void
no_lazy_lock(void)
{
	thd_t thd;

	thd_create(&thd, thd_start, NULL);
	thd_join(thd, NULL);
}

TEST_BEGIN(test_stats_arenas_small)
{
	unsigned arena;
	void *p;
	size_t sz, allocated;
	uint64_t epoch, nmalloc, ndalloc, nrequests;
	int expected = config_stats ? 0 : ENOENT;

	no_lazy_lock(); /* Lazy locking would dodge tcache testing. */

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&arena,
	    sizeof(arena)), 0, "Unexpected mallctl() failure");

	p = mallocx(SMALL_MAXCLASS, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    config_tcache ? 0 : ENOENT, "Unexpected mallctl() result");

	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.small.allocated",
	    (void *)&allocated, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.small.nmalloc", (void *)&nmalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.small.ndalloc", (void *)&ndalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.small.nrequests",
	    (void *)&nrequests, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be no greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_gt(nrequests, 0,
		    "nrequests should be greater than zero");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_large)
{
	unsigned arena;
	void *p;
	size_t sz, allocated;
	uint64_t epoch, nmalloc, ndalloc;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&arena,
	    sizeof(arena)), 0, "Unexpected mallctl() failure");

	p = mallocx((1U << LG_LARGE_MINCLASS), 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.large.allocated",
	    (void *)&allocated, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.large.nmalloc", (void *)&nmalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.large.ndalloc", (void *)&ndalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_zu_gt(allocated, 0,
		    "allocated should be greater than zero");
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_bins)
{
	unsigned arena;
	void *p;
	size_t sz, curslabs, curregs;
	uint64_t epoch, nmalloc, ndalloc, nrequests, nfills, nflushes;
	uint64_t nslabs, nreslabs;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&arena,
	    sizeof(arena)), 0, "Unexpected mallctl() failure");

	p = mallocx(arena_bin_info[0].reg_size, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("thread.tcache.flush", NULL, NULL, NULL, 0),
	    config_tcache ? 0 : ENOENT, "Unexpected mallctl() result");

	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nmalloc", (void *)&nmalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.ndalloc", (void *)&ndalloc,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nrequests",
	    (void *)&nrequests, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.curregs", (void *)&curregs,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");

	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nfills", (void *)&nfills,
	    &sz, NULL, 0), config_tcache ? expected : ENOENT,
	    "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nflushes", (void *)&nflushes,
	    &sz, NULL, 0), config_tcache ? expected : ENOENT,
	    "Unexpected mallctl() result");

	assert_d_eq(mallctl("stats.arenas.0.bins.0.nslabs", (void *)&nslabs,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.bins.0.nreslabs", (void *)&nreslabs,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");
	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.bins.0.curslabs", (void *)&curslabs,
	    &sz, NULL, 0), expected, "Unexpected mallctl() result");

	if (config_stats) {
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_gt(nrequests, 0,
		    "nrequests should be greater than zero");
		assert_zu_gt(curregs, 0,
		    "allocated should be greater than zero");
		if (config_tcache) {
			assert_u64_gt(nfills, 0,
			    "At least one fill should have occurred");
			assert_u64_gt(nflushes, 0,
			    "At least one flush should have occurred");
		}
		assert_u64_gt(nslabs, 0,
		    "At least one slab should have been allocated");
		assert_zu_gt(curslabs, 0,
		    "At least one slab should be currently allocated");
	}

	dallocx(p, 0);
}
TEST_END

TEST_BEGIN(test_stats_arenas_lextents)
{
	unsigned arena;
	void *p;
	uint64_t epoch, nmalloc, ndalloc;
	size_t curlextents, sz, hsize;
	int expected = config_stats ? 0 : ENOENT;

	arena = 0;
	assert_d_eq(mallctl("thread.arena", NULL, NULL, (void *)&arena,
	    sizeof(arena)), 0, "Unexpected mallctl() failure");

	sz = sizeof(size_t);
	assert_d_eq(mallctl("arenas.lextent.0.size", (void *)&hsize, &sz, NULL,
	    0), 0, "Unexpected mallctl() failure");

	p = mallocx(hsize, 0);
	assert_ptr_not_null(p, "Unexpected mallocx() failure");

	assert_d_eq(mallctl("epoch", NULL, NULL, (void *)&epoch, sizeof(epoch)),
	    0, "Unexpected mallctl() failure");

	sz = sizeof(uint64_t);
	assert_d_eq(mallctl("stats.arenas.0.lextents.0.nmalloc",
	    (void *)&nmalloc, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	assert_d_eq(mallctl("stats.arenas.0.lextents.0.ndalloc",
	    (void *)&ndalloc, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");
	sz = sizeof(size_t);
	assert_d_eq(mallctl("stats.arenas.0.lextents.0.curlextents",
	    (void *)&curlextents, &sz, NULL, 0), expected,
	    "Unexpected mallctl() result");

	if (config_stats) {
		assert_u64_gt(nmalloc, 0,
		    "nmalloc should be greater than zero");
		assert_u64_ge(nmalloc, ndalloc,
		    "nmalloc should be at least as large as ndalloc");
		assert_u64_gt(curlextents, 0,
		    "At least one extent should be currently allocated");
	}

	dallocx(p, 0);
}
TEST_END

int
main(void)
{
	return (test(
	    test_stats_summary,
	    test_stats_large,
	    test_stats_arenas_summary,
	    test_stats_arenas_small,
	    test_stats_arenas_large,
	    test_stats_arenas_bins,
	    test_stats_arenas_lextents));
}
