/* metalock-contested: newlib's lazy lock-creation lock under contention.
 *
 * newlib's lock_internal.c (patch 0004) creates its lock-creation lock in a start-up constructor and, for anything that
 * runs earlier, on first use.  On first use exactly one caller wins the claim and creates the lock; every other caller
 * must wait WITHOUT starving the creator, even when the creator has a lower priority than every waiter.  This row
 * builds that exact source (extracted from the patch) with its test hooks (hook.h): no start-up constructor, a hook
 * the creator runs between winning the claim and creating the lock, one after it publishes the lock, and one in every
 * pass of a loser's sleeping wait.
 *
 * Handshake: a low-priority creator (lv2 priority 3000) wins the claim and then holds in its hook until all three
 * high-priority waiters (lv2 priority 100) have reported, from inside the sleeping-wait loop, that they are waiting;
 * only then does it create and publish the lock.  So every waiter provably took the loser's path and slept there
 * while the creator still had to run, below their priority.  Checks: all three waiters reported a wait pass before
 * the creator was released, each returned after publication, every lock is created and usable.  Waiters that spun
 * instead of sleeping would keep the creator from ever running again; the row would time out. */
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
#define HOLD_LIMIT_NS (10ull * 1000000000ull)

static int claimed, all_waiting, hold_timed_out;
static uint64_t released_ns, published_ns;
static sys_lwmutex_t creator_lock, waiter_lock[WAITERS];
static pthread_t waiter_thread[WAITERS];
static int waiter_index_ready;
static unsigned wait_passes[WAITERS];
static uint64_t first_wait_ns[WAITERS], waiter_end_ns[WAITERS];

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int waiting_count(void)
{
	int n = 0;
	for (int i = 0; i < WAITERS; i++)
		n += __atomic_load_n(&wait_passes[i], __ATOMIC_ACQUIRE) != 0;
	return n;
}

/* Creator, after winning the claim: hold until every waiter is in the sleeping-wait loop. */
void test_after_claim(void)
{
	uint64_t start = now_ns();

	__atomic_store_n(&claimed, 1, __ATOMIC_RELEASE);
	while (waiting_count() < WAITERS) {
		if (now_ns() - start > HOLD_LIMIT_NS) {
			hold_timed_out = 1;
			break;
		}
		usleep(1000);
	}
	__atomic_store_n(&all_waiting, waiting_count() == WAITERS, __ATOMIC_RELEASE);
	released_ns = now_ns();
}

/* Creator, right after the release store that publishes the lock. */
void test_published(void)
{
	published_ns = now_ns();
}

/* A loser, in each pass of its sleeping wait. */
void test_waiting(void)
{
	pthread_t self = pthread_self();

	if (!__atomic_load_n(&waiter_index_ready, __ATOMIC_ACQUIRE))
		return;
	for (int i = 0; i < WAITERS; i++)
		if (pthread_equal(waiter_thread[i], self)) {
			if (__atomic_add_fetch(&wait_passes[i], 1, __ATOMIC_ACQ_REL) == 1)
				first_wait_ns[i] = now_ns();
			return;
		}
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
	/* waiter_thread[i] is stored by main before waiter_index_ready is set; wait for it so the hook can find us. */
	while (!__atomic_load_n(&waiter_index_ready, __ATOMIC_ACQUIRE))
		usleep(100);
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
	pthread_t c;
	int created[WAITERS] = { 0 };
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
	for (int i = 0; i < WAITERS; i++) {
		if ((rc = start(&waiter_thread[i], waiter, (void *)(intptr_t)i, WAITER_PRIORITY)) != 0) {
			printf("metalock-contested: waiter %d pthread_create %d FAIL\n", i, rc);
			fails++;
		} else {
			created[i] = 1;
		}
	}
	__atomic_store_n(&waiter_index_ready, 1, __ATOMIC_RELEASE);
	pthread_join(c, NULL);
	for (int i = 0; i < WAITERS; i++)
		if (created[i])
			pthread_join(waiter_thread[i], NULL);

	if (hold_timed_out || !__atomic_load_n(&all_waiting, __ATOMIC_ACQUIRE)) {
		printf("metalock-contested: only %d of %d waiters reached the sleeping wait before the creator's %llu s "
		       "limit FAIL\n", waiting_count(), WAITERS, HOLD_LIMIT_NS / 1000000000ull);
		fails++;
	}
	if (!usable(&creator_lock)) {
		printf("metalock-contested: creator's lock not usable FAIL\n");
		fails++;
	}
	uint64_t last_first_wait_ns = 0;
	for (int i = 0; i < WAITERS; i++)
		if (created[i] && first_wait_ns[i] > last_first_wait_ns)
			last_first_wait_ns = first_wait_ns[i];
	printf("metalock-contested: creator released %lld us after the last waiter's first wait pass, published %lld us "
	       "after release\n", (long long)(released_ns - last_first_wait_ns) / 1000,
	       (long long)(published_ns - released_ns) / 1000);
	for (int i = 0; i < WAITERS; i++) {
		if (!created[i])
			continue;
		int waited = wait_passes[i] != 0 && first_wait_ns[i] <= released_ns && waiter_end_ns[i] >= published_ns;
		printf("metalock-contested: waiter %d: %u sleeping-wait passes, first %lld us before the creator's release, "
		       "returned %lld us after publication\n", i, wait_passes[i],
		       (long long)(released_ns - first_wait_ns[i]) / 1000, (long long)(waiter_end_ns[i] - published_ns) / 1000);
		if (!waited) {
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
