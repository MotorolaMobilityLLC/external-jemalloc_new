#include "test/jemalloc_test.h"

#define THREAD_DATA 0x72b65c10

typedef unsigned int data_t;

static bool data_cleanup_executed;
static bool data_test_started;

malloc_tsd_types(data_, data_t)
malloc_tsd_protos(, data_, data_t)

void
data_cleanup(void *arg) {
	data_t *data = (data_t *)arg;

	if (!data_test_started) {
		return;
	}
	if (!data_cleanup_executed) {
		assert_x_eq(*data, THREAD_DATA,
		    "Argument passed into cleanup function should match tsd "
		    "value");
	}
	data_cleanup_executed = true;

	/*
	 * Allocate during cleanup for two rounds, in order to assure that
	 * jemalloc's internal tsd reinitialization happens.
	 */
	switch (*data) {
	case THREAD_DATA:
		*data = 1;
		data_tsd_set(data);
		break;
	case 1:
		*data = 2;
		data_tsd_set(data);
		break;
	case 2:
		return;
	default:
		not_reached();
	}

	{
		void *p = mallocx(1, 0);
		assert_ptr_not_null(p, "Unexpeced mallocx() failure");
		dallocx(p, 0);
	}
}

malloc_tsd_externs(data_, data_t)
#define DATA_INIT 0x12345678
malloc_tsd_data(, data_, data_t, DATA_INIT)
malloc_tsd_funcs(, data_, data_t, DATA_INIT, data_cleanup)

static void *
thd_start(void *arg) {
	data_t d = (data_t)(uintptr_t)arg;
	void *p;

	assert_x_eq(*data_tsd_get(true), DATA_INIT,
	    "Initial tsd get should return initialization value");

	p = malloc(1);
	assert_ptr_not_null(p, "Unexpected malloc() failure");

	data_tsd_set(&d);
	assert_x_eq(*data_tsd_get(true), d,
	    "After tsd set, tsd get should return value that was set");

	d = 0;
	assert_x_eq(*data_tsd_get(true), (data_t)(uintptr_t)arg,
	    "Resetting local data should have no effect on tsd");

	free(p);
	return NULL;
}

TEST_BEGIN(test_tsd_main_thread) {
	thd_start((void *)(uintptr_t)0xa5f3e329);
}
TEST_END

TEST_BEGIN(test_tsd_sub_thread) {
	thd_t thd;

	data_cleanup_executed = false;
	thd_create(&thd, thd_start, (void *)THREAD_DATA);
	thd_join(thd, NULL);
	assert_true(data_cleanup_executed,
	    "Cleanup function should have executed");
}
TEST_END

static void *
thd_start_reincarnated(void *arg) {
	tsd_t *tsd = tsd_fetch();
	assert(tsd);

	void *p = malloc(1);
	assert_ptr_not_null(p, "Unexpected malloc() failure");

	/* Manually trigger reincarnation. */
	assert_ptr_not_null(tsd->arena, "Should have tsd arena set.");
	tsd_cleanup((void *)tsd);
	assert_ptr_null(tsd->arena, "TSD arena should have been cleared.");
	assert_u_eq(tsd->state, tsd_state_purgatory,
	    "TSD state should be purgatory\n");

	free(p);
	assert_u_eq(tsd->state, tsd_state_reincarnated,
	    "TSD state should be reincarnated\n");
	p = mallocx(1, MALLOCX_TCACHE_NONE);
	assert_ptr_not_null(p, "Unexpected malloc() failure");
	assert_ptr_not_null(tsd->arena,
	    "Should have tsd arena set after reincarnation.");

	free(p);
	tsd_cleanup((void *)tsd);
	assert_ptr_null(tsd->arena,
	    "TSD arena should have been cleared after 2nd cleanup.");

	return NULL;
}

TEST_BEGIN(test_tsd_reincarnation) {
	thd_t thd;
	thd_create(&thd, thd_start_reincarnated, NULL);
	thd_join(thd, NULL);
}
TEST_END

int
main(void) {
	/* Core tsd bootstrapping must happen prior to data_tsd_boot(). */
	if (nallocx(1, 0) == 0) {
		malloc_printf("Initialization error");
		return test_status_fail;
	}
	data_test_started = false;
	data_tsd_boot();
	data_test_started = true;

	return test_no_reentrancy(
	    test_tsd_main_thread,
	    test_tsd_sub_thread,
	    test_tsd_reincarnation);
}
