/* metalock-contested: newlib's lazy lock-creation lock under contention.
 *
 * newlib's lock_internal.c (patch 0004) creates its lock-creation lock in a start-up constructor and, for anything that
 * runs earlier, on first use.  On first use exactly one caller wins the claim and creates the lock; every other caller
 * must wait WITHOUT starving the creator, even when the creator has a lower priority than every waiter.  This row
 * builds that exact source (extracted from the patch) with its test hooks: no start-up constructor, and a delay hook
 * the creator runs between winning the claim and creating the lock.
 *
 * Deterministic schedule: a low-priority creator (lv2 priority 3000) wins the claim and sleeps 200 ms in the hook;
 * main waits for the claim, then starts three high-priority waiters (lv2 priority 100), which find the lock still
 * being created.  Checks: every call returns, all four locks are created and usable, every waiter started before the
 * lock was published and returned after it.  Waiters that spun instead of sleeping could keep the creator from ever
 * running again; this row would then time out. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/lock.h>
#include <time.h>
#include <unistd.h>

typedef struct _sys_lwmutex sys_lwmutex_t;
extern void test_auto_lock_allocate(sys_lwmutex_t *lwmutex);
extern int32_t sys_lwmutex_lock(sys_lwmutex_t *lwmutex, uint64_t timeout);
extern int32_t sys_lwmutex_unlock(sys_lwmutex_t *lwmutex);

enum { WAITERS = 3, CREATOR_PRIORITY = 3000, WAITER_PRIORITY = 100 };

static int claimed;
static uint64_t published_ns;
static sys_lwmutex_t creator_lock, waiter_lock[WAITERS];
static uint64_t waiter_start_ns[WAITERS], waiter_end_ns[WAITERS];

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Runs in the creator after it wins the claim, before it creates the lock. */
void test_after_claim(void)
{
	__atomic_store_n(&claimed, 1, __ATOMIC_RELEASE);
	usleep(200000);
	published_ns = now_ns();	/* creation and publication follow immediately */
}

static void *creator(void *arg)
{
	(void)arg;
	test_auto_lock_allocate(&creator_lock);
	return NULL;
}

static void *waiter(void *arg)
{
	int i = (int)(intptr_t)arg;
	waiter_start_ns[i] = now_ns();
	test_auto_lock_allocate(&waiter_lock[i]);
	waiter_end_ns[i] = now_ns();
	return NULL;
}

static int start(pthread_t *t, void *(*fn)(void *), void *arg, int priority)
{
	pthread_attr_t attr;
	struct sched_param param = { 0 };
	int rc = pthread_attr_init(&attr);
	param.sched_priority = priority;	/* lv2 priority, the shim's documented extension */
	if (!rc)
		rc = pthread_attr_setschedparam(&attr, &param);
	if (!rc)
		rc = pthread_create(t, &attr, fn, arg);
	pthread_attr_destroy(&attr);
	return rc;
}

static int usable(sys_lwmutex_t *l)
{
	return l->attribute != 0 && sys_lwmutex_lock(l, 0) == 0 && sys_lwmutex_unlock(l) == 0;
}

int main(void)
{
	pthread_t c, w[WAITERS];
	int fails = 0, rc;

	printf("metalock-contested: creator priority %d, %d waiters priority %d\n", CREATOR_PRIORITY, WAITERS,
	       WAITER_PRIORITY);
	if ((rc = start(&c, creator, NULL, CREATOR_PRIORITY)) != 0) {
		printf("metalock-contested: creator pthread_create %d FAIL\n", rc);
		printf("METALOCK_CONTESTED_FAIL 1\n");
		return 1;
	}
	while (!__atomic_load_n(&claimed, __ATOMIC_ACQUIRE))
		usleep(1000);
	for (int i = 0; i < WAITERS; i++)
		if ((rc = start(&w[i], waiter, (void *)(intptr_t)i, WAITER_PRIORITY)) != 0) {
			printf("metalock-contested: waiter %d pthread_create %d FAIL\n", i, rc);
			fails++;
		}
	pthread_join(c, NULL);
	for (int i = 0; i < WAITERS; i++)
		pthread_join(w[i], NULL);

	if (!usable(&creator_lock)) {
		printf("metalock-contested: creator's lock not usable FAIL\n");
		fails++;
	}
	for (int i = 0; i < WAITERS; i++) {
		int contended = waiter_start_ns[i] < published_ns && waiter_end_ns[i] >= published_ns;
		printf("metalock-contested: waiter %d started %lld us before publication, returned %lld us after\n", i,
		       (long long)(published_ns - waiter_start_ns[i]) / 1000,
		       (long long)(waiter_end_ns[i] - published_ns) / 1000);
		if (!contended) {
			printf("metalock-contested: waiter %d did not wait on the creator FAIL\n", i);
			fails++;
		}
		if (!usable(&waiter_lock[i])) {
			printf("metalock-contested: waiter %d lock not usable FAIL\n", i);
			fails++;
		}
	}
	if (fails)
		printf("METALOCK_CONTESTED_FAIL %d\n", fails);
	else
		printf("METALOCK_CONTESTED_OK\n");
	return fails;
}
