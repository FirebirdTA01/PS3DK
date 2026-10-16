/* librt's clock-selecting waits (POSIX Issue 8: pthread_cond_clockwait, pthread_mutex_clocklock,
   pthread_rwlock_clockrdlock / clockwrlock) and sysconf.  libstdc++ reaches the first three through
   condition_variable, timed_mutex and shared_timed_mutex waits on steady_clock (CLOCK_MONOTONIC).
   Each wait must time out after its deadline on CLOCK_MONOTONIC and on CLOCK_REALTIME while another thread
   holds the lock, must reject any other clock with EINVAL, and must succeed once the lock is free. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define WAIT_MS 50
#define BAD_CLOCK ((clockid_t)12345)

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("pthread-clockwait: FAIL %s\n", what);
		failures++;
	}
}

static long long mono_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static struct timespec deadline(clockid_t clock, int ms)
{
	struct timespec t;

	clock_gettime(clock, &t);
	t.tv_nsec += (long)(ms % 1000) * 1000000;
	t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000;
	t.tv_nsec %= 1000000000;
	return t;
}

/* A wait that must time out: ETIMEDOUT, after at least WAIT_MS and well before 2 s. */
static void expect_timeout(int rc, long long start, const char *what)
{
	long long took = mono_ms() - start;
	char msg[96];

	snprintf(msg, sizeof msg, "%s: rc %d, %lld ms", what, rc, took);
	check(rc == ETIMEDOUT && took >= WAIT_MS - 1 && took < 2000, msg);
}

static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t gate_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_c = PTHREAD_COND_INITIALIZER;
static int holding, release_now;

/* Holds HELD and RW (for writing) until main says release. */
static void *holder(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&held);
	pthread_rwlock_wrlock(&rw);
	pthread_mutex_lock(&gate_m);
	holding = 1;
	pthread_cond_broadcast(&gate_c);
	while (!release_now)
		pthread_cond_wait(&gate_c, &gate_m);
	pthread_mutex_unlock(&gate_m);
	pthread_rwlock_unlock(&rw);
	pthread_mutex_unlock(&held);
	return NULL;
}

static pthread_mutex_t sig_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sig_c = PTHREAD_COND_INITIALIZER;
static int signalled;

/* Signals sig_c after 100 ms. */
static void *signaller(void *arg)
{
	(void)arg;
	usleep(100000);
	pthread_mutex_lock(&sig_m);
	signalled = 1;
	pthread_cond_broadcast(&sig_c);
	pthread_mutex_unlock(&sig_m);
	return NULL;
}

/* Deadline edge cases on cond_clockwait / cond_timedwait, both clocks:
   - invalid nanoseconds (-1, 1000000000): EINVAL with no wait, and the mutex is still held afterwards;
   - an expired deadline and a sub-microsecond one: ETIMEDOUT, promptly;
   - a far-future deadline (seconds near 2^62, which overflowed the old microsecond arithmetic into a 1 us wait):
     the wait lasts until another thread signals, about 100 ms later. */
static void deadline_cases(pthread_mutex_t *m, pthread_cond_t *c)
{
	static const clockid_t clocks[] = { CLOCK_MONOTONIC, CLOCK_REALTIME };
	static const char *names[] = { "MONOTONIC", "REALTIME" };
	static const long bad_nsec[] = { -1, 1000000000L };
	struct timespec t;
	char what[96];
	long long start;
	int i, k, rc;

	for (i = 0; i < 2; i++) {
		for (k = 0; k < 2; k++) {
			pthread_mutex_lock(m);
			t = deadline(clocks[i], WAIT_MS);
			t.tv_nsec = bad_nsec[k];
			start = mono_ms();
			rc = pthread_cond_clockwait(c, m, clocks[i], &t);
			snprintf(what, sizeof what, "cond_clockwait %s nsec %ld: rc %d, %lld ms", names[i], bad_nsec[k],
			         rc, mono_ms() - start);
			check(rc == EINVAL && mono_ms() - start < WAIT_MS, what);
			snprintf(what, sizeof what, "cond_clockwait %s nsec %ld: mutex still held", names[i], bad_nsec[k]);
			check(pthread_mutex_trylock(m) == EBUSY, what);
			if (clocks[i] == CLOCK_REALTIME) {
				snprintf(what, sizeof what, "cond_timedwait nsec %ld", bad_nsec[k]);
				check(pthread_cond_timedwait(c, m, &t) == EINVAL, what);
			}
			pthread_mutex_unlock(m);
		}

		/* Expired by a second, and less than a microsecond ahead. */
		pthread_mutex_lock(m);
		clock_gettime(clocks[i], &t);
		t.tv_sec -= 1;
		start = mono_ms();
		rc = pthread_cond_clockwait(c, m, clocks[i], &t);
		snprintf(what, sizeof what, "cond_clockwait %s expired: rc %d, %lld ms", names[i], rc, mono_ms() - start);
		check(rc == ETIMEDOUT && mono_ms() - start < 500, what);
		clock_gettime(clocks[i], &t);
		t.tv_nsec += 500;
		if (t.tv_nsec >= 1000000000L) {
			t.tv_nsec -= 1000000000L;
			t.tv_sec++;
		}
		start = mono_ms();
		rc = pthread_cond_clockwait(c, m, clocks[i], &t);
		snprintf(what, sizeof what, "cond_clockwait %s 500 ns ahead: rc %d, %lld ms", names[i], rc,
		         mono_ms() - start);
		check(rc == ETIMEDOUT && mono_ms() - start < 500, what);
		pthread_mutex_unlock(m);

		/* Far future: only the signal ends the wait. */
		{
			pthread_t th;

			signalled = 0;
			check(pthread_create(&th, NULL, signaller, NULL) == 0, "pthread_create signaller");
			pthread_mutex_lock(&sig_m);
			t.tv_sec = (time_t)1 << 62;
			t.tv_nsec = 0;
			start = mono_ms();
			rc = 0;
			while (!signalled && rc == 0)
				rc = pthread_cond_clockwait(&sig_c, &sig_m, clocks[i], &t);
			pthread_mutex_unlock(&sig_m);
			snprintf(what, sizeof what, "cond_clockwait %s far future: rc %d, signalled %d, %lld ms", names[i],
			         rc, signalled, mono_ms() - start);
			check(rc == 0 && signalled && mono_ms() - start >= 90, what);
			pthread_join(th, NULL);
		}
	}
}

int main(void)
{
	static const clockid_t clocks[] = { CLOCK_MONOTONIC, CLOCK_REALTIME };
	static const char *names[] = { "MONOTONIC", "REALTIME" };
	pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
	pthread_cond_t c = PTHREAD_COND_INITIALIZER;
	struct timespec t;
	pthread_t th;
	char what[80];
	long long start;
	int i, rc;

	/* sysconf: the PPE's two hardware threads; an unsupported name is EINVAL. */
	check(sysconf(_SC_NPROCESSORS_ONLN) == 2, "sysconf(_SC_NPROCESSORS_ONLN) != 2");
	check(sysconf(_SC_NPROCESSORS_CONF) == 2, "sysconf(_SC_NPROCESSORS_CONF) != 2");
	errno = 0;
	check(sysconf(-12345) == -1 && errno == EINVAL, "sysconf(unknown) is not -1/EINVAL");

	/* cond_clockwait with nobody signalling. */
	for (i = 0; i < 2; i++) {
		pthread_mutex_lock(&m);
		t = deadline(clocks[i], WAIT_MS);
		start = mono_ms();
		rc = pthread_cond_clockwait(&c, &m, clocks[i], &t);
		snprintf(what, sizeof what, "cond_clockwait %s", names[i]);
		expect_timeout(rc, start, what);
		pthread_mutex_unlock(&m);
	}
	pthread_mutex_lock(&m);
	t = deadline(CLOCK_MONOTONIC, WAIT_MS);
	check(pthread_cond_clockwait(&c, &m, BAD_CLOCK, &t) == EINVAL,
	      "cond_clockwait on an unknown clock is not EINVAL");
	pthread_mutex_unlock(&m);

	/* Locks held by another thread. */
	check(pthread_create(&th, NULL, holder, NULL) == 0, "pthread_create");
	pthread_mutex_lock(&gate_m);
	while (!holding)
		pthread_cond_wait(&gate_c, &gate_m);
	pthread_mutex_unlock(&gate_m);
	for (i = 0; i < 2; i++) {
		t = deadline(clocks[i], WAIT_MS);
		start = mono_ms();
		rc = pthread_mutex_clocklock(&held, clocks[i], &t);
		snprintf(what, sizeof what, "mutex_clocklock %s", names[i]);
		expect_timeout(rc, start, what);

		t = deadline(clocks[i], WAIT_MS);
		start = mono_ms();
		rc = pthread_rwlock_clockrdlock(&rw, clocks[i], &t);
		snprintf(what, sizeof what, "rwlock_clockrdlock %s", names[i]);
		expect_timeout(rc, start, what);

		t = deadline(clocks[i], WAIT_MS);
		start = mono_ms();
		rc = pthread_rwlock_clockwrlock(&rw, clocks[i], &t);
		snprintf(what, sizeof what, "rwlock_clockwrlock %s", names[i]);
		expect_timeout(rc, start, what);
	}
	t = deadline(CLOCK_MONOTONIC, WAIT_MS);
	check(pthread_mutex_clocklock(&held, BAD_CLOCK, &t) == EINVAL,
	      "mutex_clocklock on an unknown clock is not EINVAL");
	check(pthread_rwlock_clockrdlock(&rw, BAD_CLOCK, &t) == EINVAL,
	      "rwlock_clockrdlock on an unknown clock is not EINVAL");

	/* Invalid nanoseconds on a lock that must be waited for: EINVAL, at once. */
	for (i = 0; i < 2; i++) {
		static const long bad_nsec[] = { -1, 1000000000L };
		int k;

		for (k = 0; k < 2; k++) {
			t = deadline(clocks[i], WAIT_MS);
			t.tv_nsec = bad_nsec[k];
			snprintf(what, sizeof what, "held, nsec %ld, %s: mutex_clocklock", bad_nsec[k], names[i]);
			check(pthread_mutex_clocklock(&held, clocks[i], &t) == EINVAL, what);
			snprintf(what, sizeof what, "held, nsec %ld, %s: rwlock_clockrdlock", bad_nsec[k], names[i]);
			check(pthread_rwlock_clockrdlock(&rw, clocks[i], &t) == EINVAL, what);
			snprintf(what, sizeof what, "held, nsec %ld, %s: rwlock_clockwrlock", bad_nsec[k], names[i]);
			check(pthread_rwlock_clockwrlock(&rw, clocks[i], &t) == EINVAL, what);
			if (clocks[i] == CLOCK_REALTIME) {
				snprintf(what, sizeof what, "held, nsec %ld: mutex_timedlock", bad_nsec[k]);
				check(pthread_mutex_timedlock(&held, &t) == EINVAL, what);
			}
		}
	}

	pthread_mutex_lock(&gate_m);
	release_now = 1;
	pthread_cond_broadcast(&gate_c);
	pthread_mutex_unlock(&gate_m);
	check(pthread_join(th, NULL) == 0, "pthread_join");

	/* Free now: each succeeds well before its deadline. */
	t = deadline(CLOCK_MONOTONIC, 1000);
	check(pthread_mutex_clocklock(&held, CLOCK_MONOTONIC, &t) == 0, "mutex_clocklock on a free mutex");
	pthread_mutex_unlock(&held);
	t = deadline(CLOCK_MONOTONIC, 1000);
	check(pthread_rwlock_clockrdlock(&rw, CLOCK_MONOTONIC, &t) == 0, "rwlock_clockrdlock on a free lock");
	pthread_rwlock_unlock(&rw);
	t = deadline(CLOCK_MONOTONIC, 1000);
	check(pthread_rwlock_clockwrlock(&rw, CLOCK_MONOTONIC, &t) == 0, "rwlock_clockwrlock on a free lock");
	pthread_rwlock_unlock(&rw);

	/* A lock that can be taken at once is taken, even with an invalid deadline (POSIX: its validity need not
	   be checked then). */
	t = deadline(CLOCK_MONOTONIC, WAIT_MS);
	t.tv_nsec = -1;
	check(pthread_mutex_clocklock(&held, CLOCK_MONOTONIC, &t) == 0, "free mutex, invalid nsec: acquired");
	pthread_mutex_unlock(&held);
	check(pthread_rwlock_clockwrlock(&rw, CLOCK_MONOTONIC, &t) == 0, "free rwlock, invalid nsec: acquired");
	pthread_rwlock_unlock(&rw);

	deadline_cases(&m, &c);

	puts(failures ? "PTHREAD_CLOCKWAIT_FAIL" : "PTHREAD_CLOCKWAIT_OK");
	return failures != 0;
}
