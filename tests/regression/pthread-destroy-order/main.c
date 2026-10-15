/* pthread-destroy-order: a mutex and the condition variable used with it can be destroyed in either order.
 *
 * Lv-2 binds a condition variable to one mutex when it is created and will not destroy that mutex while the
 * condition variable exists; POSIX lets a program destroy the two in either order (miniaudio's teardown destroys
 * the mutex first).  librt's shim destroys the idle conds bound to a mutex before the mutex itself.
 *
 *  1. wait/signal on (m, c), then pthread_mutex_destroy(m) before pthread_cond_destroy(c): both return 0;
 *  2. a cond whose mutex was destroyed is reused with a new mutex: a timed wait times out (the cond works);
 *  3. pthread_mutex_destroy while a thread is blocked in a wait on a cond bound to that mutex returns EBUSY and
 *     leaves both usable: the waiter is then woken, joined, and both destroy cleanly. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>

static pthread_mutex_t m;
static pthread_cond_t c;
static int waiting, go;
static int fails;

static void check(const char *what, int got, int want)
{
	if (got != want) {
		printf("pthread-destroy-order: %s returned %d, expected %d FAIL\n", what, got, want);
		fails++;
	} else {
		printf("pthread-destroy-order: %s = %d ok\n", what, got);
	}
}

/* Waits on (m, c) until main sets go. */
static void *waiter(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&m);
	waiting = 1;
	while (!go)
		pthread_cond_wait(&c, &m);
	pthread_mutex_unlock(&m);
	return NULL;
}

/* Starts the waiter and returns once it is blocked in pthread_cond_wait: holding m with waiting set means the
 * waiter released m inside the wait. */
static int start_blocked_waiter(pthread_t *t)
{
	int rc;

	waiting = 0;
	go = 0;
	if ((rc = pthread_create(t, NULL, waiter, NULL)) != 0)
		return rc;
	for (;;) {
		pthread_mutex_lock(&m);
		if (waiting) {
			pthread_mutex_unlock(&m);
			return 0;
		}
		pthread_mutex_unlock(&m);
		struct timespec ms = { 0, 1000000L };
		nanosleep(&ms, NULL);
	}
}

static void release_waiter(pthread_t t)
{
	pthread_mutex_lock(&m);
	go = 1;
	pthread_cond_signal(&c);
	pthread_mutex_unlock(&m);
	pthread_join(t, NULL);
}

int main(void)
{
	pthread_t t;
	pthread_mutex_t m2;
	struct timespec abstime;

	/* 1. mutex first */
	check("case 1 mutex_init", pthread_mutex_init(&m, NULL), 0);
	check("case 1 cond_init", pthread_cond_init(&c, NULL), 0);
	check("case 1 create", start_blocked_waiter(&t), 0);
	release_waiter(t);
	check("case 1 mutex_destroy before cond_destroy", pthread_mutex_destroy(&m), 0);
	check("case 1 cond_destroy after mutex_destroy", pthread_cond_destroy(&c), 0);

	/* 2. the cond outlives its mutex and is reused with another one */
	check("case 2 mutex_init", pthread_mutex_init(&m, NULL), 0);
	check("case 2 cond_init", pthread_cond_init(&c, NULL), 0);
	check("case 2 create", start_blocked_waiter(&t), 0);
	release_waiter(t);
	check("case 2 mutex_destroy", pthread_mutex_destroy(&m), 0);
	check("case 2 second mutex_init", pthread_mutex_init(&m2, NULL), 0);
	clock_gettime(CLOCK_REALTIME, &abstime);
	abstime.tv_nsec += 20000000L;
	if (abstime.tv_nsec >= 1000000000L) {
		abstime.tv_sec++;
		abstime.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&m2);
	check("case 2 timedwait with the second mutex", pthread_cond_timedwait(&c, &m2, &abstime), ETIMEDOUT);
	pthread_mutex_unlock(&m2);
	check("case 2 cond_destroy", pthread_cond_destroy(&c), 0);
	check("case 2 second mutex_destroy", pthread_mutex_destroy(&m2), 0);

	/* 3. a waiter makes the mutex busy */
	check("case 3 mutex_init", pthread_mutex_init(&m, NULL), 0);
	check("case 3 cond_init", pthread_cond_init(&c, NULL), 0);
	check("case 3 create", start_blocked_waiter(&t), 0);
	check("case 3 mutex_destroy with a waiter", pthread_mutex_destroy(&m), EBUSY);
	release_waiter(t);
	check("case 3 cond_destroy", pthread_cond_destroy(&c), 0);
	check("case 3 mutex_destroy", pthread_mutex_destroy(&m), 0);

	if (fails)
		printf("PTHREAD_DESTROY_ORDER_FAIL %d\n", fails);
	else
		printf("PTHREAD_DESTROY_ORDER_OK\n");
	return fails;
}
