/* pthread-rwlock: librt's POSIX read-write locks over the Lv-2 rwlock.
 *
 * Checks, with a statically initialised lock and with pthread_rwlock_init:
 *  - two readers hold the lock at the same time (each waits, holding, for the other to arrive);
 *  - a held read lock makes trywrlock return EBUSY and timedwrlock time out, and unlock releases it;
 *  - a held write lock makes tryrdlock and trywrlock return EBUSY, and unlock releases it;
 *  - a writer blocked behind a reader gets the lock once the reader unlocks, and excludes readers;
 *  - destroy of a held lock is refused, destroy of a free one succeeds;
 *  - the attribute functions accept process-private and refuse process-shared. */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static pthread_rwlock_t s_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_rwlock_t *lk;
static int readers_in, writer_done, writer_saw_readers = -1;
static int fails;

static void check(const char *what, int got, int want)
{
	printf("pthread-rwlock: %s = %d%s\n", what, got, got == want ? " ok" : " FAIL");
	if (got != want)
		fails++;
}

static int wait_for(int *flag, int value)
{
	for (int i = 0; i < 5000; i++) {
		if (__atomic_load_n(flag, __ATOMIC_ACQUIRE) == value)
			return 1;
		usleep(1000);
	}
	return 0;
}

/* Holds a read lock until both readers are inside. */
static void *reader(void *arg)
{
	(void)arg;
	if (pthread_rwlock_rdlock(lk) != 0)
		return (void *)1;
	__atomic_add_fetch(&readers_in, 1, __ATOMIC_ACQ_REL);
	int both = wait_for(&readers_in, 2);
	pthread_rwlock_unlock(lk);
	return (void *)(long)!both;
}

static void *writer(void *arg)
{
	(void)arg;
	if (pthread_rwlock_wrlock(lk) != 0)
		return (void *)1;
	writer_saw_readers = __atomic_load_n(&readers_in, __ATOMIC_ACQUIRE);
	__atomic_store_n(&writer_done, 1, __ATOMIC_RELEASE);
	pthread_rwlock_unlock(lk);
	return NULL;
}

static void round_on(const char *name, pthread_rwlock_t *l)
{
	pthread_t a, b, w;
	void *ra, *rb;
	char what[96];
	struct timespec abstime;

	lk = l;
	readers_in = 0;
	writer_done = 0;

	/* Two readers at once. */
	pthread_create(&a, NULL, reader, NULL);
	pthread_create(&b, NULL, reader, NULL);
	pthread_join(a, &ra);
	pthread_join(b, &rb);
	snprintf(what, sizeof what, "%s: both readers held it together", name);
	check(what, ra == NULL && rb == NULL, 1);

	/* A read hold blocks writers. */
	check("rdlock", pthread_rwlock_rdlock(l), 0);
	snprintf(what, sizeof what, "%s: trywrlock under a read hold", name);
	check(what, pthread_rwlock_trywrlock(l), EBUSY);
	clock_gettime(CLOCK_REALTIME, &abstime);
	abstime.tv_nsec += 20000000L;
	if (abstime.tv_nsec >= 1000000000L) {
		abstime.tv_sec++;
		abstime.tv_nsec -= 1000000000L;
	}
	snprintf(what, sizeof what, "%s: timedwrlock under a read hold", name);
	check(what, pthread_rwlock_timedwrlock(l, &abstime), ETIMEDOUT);
	snprintf(what, sizeof what, "%s: destroy while read-held", name);
	check(what, pthread_rwlock_destroy(l) != 0, 1);

	/* A writer queued behind the read hold gets it after the unlock. */
	readers_in = 0;
	pthread_create(&w, NULL, writer, NULL);
	usleep(20000);
	snprintf(what, sizeof what, "%s: writer still waiting behind the reader", name);
	check(what, __atomic_load_n(&writer_done, __ATOMIC_ACQUIRE), 0);
	check("read unlock", pthread_rwlock_unlock(l), 0);
	pthread_join(w, NULL);
	snprintf(what, sizeof what, "%s: writer ran after the reader left", name);
	check(what, writer_done && writer_saw_readers == 0, 1);

	/* A write hold blocks everyone. */
	check("wrlock", pthread_rwlock_wrlock(l), 0);
	snprintf(what, sizeof what, "%s: tryrdlock under a write hold", name);
	check(what, pthread_rwlock_tryrdlock(l), EBUSY);
	snprintf(what, sizeof what, "%s: trywrlock under a write hold", name);
	check(what, pthread_rwlock_trywrlock(l), EBUSY);
	check("write unlock", pthread_rwlock_unlock(l), 0);
	snprintf(what, sizeof what, "%s: tryrdlock after the write unlock", name);
	check(what, pthread_rwlock_tryrdlock(l), 0);
	check("read unlock", pthread_rwlock_unlock(l), 0);
}

int main(void)
{
	pthread_rwlockattr_t attr;
	pthread_rwlock_t d_lock;
	int pshared = -1;

	round_on("static", &s_lock);

	check("rwlockattr_init", pthread_rwlockattr_init(&attr), 0);
	check("setpshared private", pthread_rwlockattr_setpshared(&attr, 0), 0);
	check("setpshared shared refused", pthread_rwlockattr_setpshared(&attr, 1), ENOTSUP);
	check("getpshared", pthread_rwlockattr_getpshared(&attr, &pshared), 0);
	check("pshared value", pshared, 0);
	check("rwlock_init", pthread_rwlock_init(&d_lock, &attr), 0);
	check("rwlockattr_destroy", pthread_rwlockattr_destroy(&attr), 0);
	round_on("dynamic", &d_lock);

	check("destroy static", pthread_rwlock_destroy(&s_lock), 0);
	check("destroy dynamic", pthread_rwlock_destroy(&d_lock), 0);

	if (fails)
		printf("PTHREAD_RWLOCK_FAIL %d\n", fails);
	else
		printf("PTHREAD_RWLOCK_OK\n");
	return fails;
}
