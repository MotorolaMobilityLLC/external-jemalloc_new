#define	JEMALLOC_MUTEX_C_
#include "jemalloc/internal/jemalloc_internal.h"

#ifdef JEMALLOC_LAZY_LOCK
#include <dlfcn.h>
#endif

/******************************************************************************/
/* Data. */

#ifdef JEMALLOC_LAZY_LOCK
bool isthreaded = false;
#endif

#ifdef JEMALLOC_LAZY_LOCK
static void	pthread_create_once(void);
#endif

/******************************************************************************/
/*
 * We intercept pthread_create() calls in order to toggle isthreaded if the
 * process goes multi-threaded.
 */

#ifdef JEMALLOC_LAZY_LOCK
static int (*pthread_create_fptr)(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);

static void
pthread_create_once(void)
{

	pthread_create_fptr = dlsym(RTLD_NEXT, "pthread_create");
	if (pthread_create_fptr == NULL) {
		malloc_write("<jemalloc>: Error in dlsym(RTLD_NEXT, "
		    "\"pthread_create\")\n");
		abort();
	}

	isthreaded = true;
}

JEMALLOC_ATTR(visibility("default"))
int
pthread_create(pthread_t *__restrict thread,
    const pthread_attr_t *__restrict attr, void *(*start_routine)(void *),
    void *__restrict arg)
{
	static pthread_once_t once_control = PTHREAD_ONCE_INIT;

	pthread_once(&once_control, pthread_create_once);

	return (pthread_create_fptr(thread, attr, start_routine, arg));
}
#endif

/******************************************************************************/

bool
malloc_mutex_init(malloc_mutex_t *mutex)
{
#ifdef JEMALLOC_OSSPIN
	*mutex = 0;
#else
	pthread_mutexattr_t attr;

	if (pthread_mutexattr_init(&attr) != 0)
		return (true);
#ifdef PTHREAD_MUTEX_ADAPTIVE_NP
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
#else
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
#endif
	if (pthread_mutex_init(mutex, &attr) != 0) {
		pthread_mutexattr_destroy(&attr);
		return (true);
	}
	pthread_mutexattr_destroy(&attr);

#endif
	return (false);
}

void
malloc_mutex_destroy(malloc_mutex_t *mutex)
{

#ifndef JEMALLOC_OSSPIN
	if (pthread_mutex_destroy(mutex) != 0) {
		malloc_write("<jemalloc>: Error in pthread_mutex_destroy()\n");
		abort();
	}
#endif
}

void
malloc_mutex_prefork(malloc_mutex_t *mutex)
{

	malloc_mutex_lock(mutex);
}

void
malloc_mutex_postfork_parent(malloc_mutex_t *mutex)
{

	malloc_mutex_unlock(mutex);
}

void
malloc_mutex_postfork_child(malloc_mutex_t *mutex)
{

	if (malloc_mutex_init(mutex)) {
		malloc_printf("<jemalloc>: Error re-initializing mutex in "
		    "child\n");
		if (opt_abort)
			abort();
	}
}
