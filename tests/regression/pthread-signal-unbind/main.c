/* pthread-signal-unbind: a signal that loaded a cond's id races pthread_mutex_destroy of the cond's old mutex.
 *
 * pthread_cond_signal does not need the mutex, so a signaller can hold the Lv-2 id of an idle cond while another
 * thread destroys the mutex that cond is bound to; librt then destroys the kernel cond with it.  The destroy must
 * wait for the in-flight signal, or the signal reaches a destroyed (or recycled) id and returns ESRCH.
 *
 * Deterministic schedule, with librt's pthread.c built into the row and a hook between the id load and the syscall:
 * the cond is bound to m by a timed wait; the signaller loads the id, reports it, and sleeps 100 ms in the hook; main
 * then calls pthread_mutex_destroy(m).  Checks: the signal returns 0, the destroy returns 0 and returns only after
 * the signal's syscall finished, and the cond is then reusable with another mutex. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t m, m2;
static pthread_cond_t c;
static int loaded, hook_armed, signal_rc = -1, fails;
static uint64_t signal_end_ns;

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

/* Runs in pthread_cond_signal after the id is loaded, before the syscall. */
void test_cond_id_loaded(void)
{
	if (!__atomic_load_n(&hook_armed, __ATOMIC_ACQUIRE))
		return;
	__atomic_store_n(&loaded, 1, __ATOMIC_RELEASE);
	usleep(100000);
}

static void *signaller(void *arg)
{
	(void)arg;
	signal_rc = pthread_cond_signal(&c);
	signal_end_ns = now_ns();
	return NULL;
}

static int timed_wait_ms(pthread_mutex_t *mx, int ms)
{
	struct timespec abstime;
	int rc;

	clock_gettime(CLOCK_REALTIME, &abstime);
	abstime.tv_nsec += ms * 1000000L;
	while (abstime.tv_nsec >= 1000000000L) {
		abstime.tv_sec++;
		abstime.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(mx);
	rc = pthread_cond_timedwait(&c, mx, &abstime);
	pthread_mutex_unlock(mx);
	return rc;
}

static void check(const char *what, long long got, long long want)
{
	printf("pthread-signal-unbind: %s = %lld%s\n", what, got, got == want ? " ok" : " FAIL");
	if (got != want)
		fails++;
}

int main(void)
{
	pthread_t t;
	uint64_t destroy_end_ns;
	int rc;

	check("mutex_init", pthread_mutex_init(&m, NULL), 0);
	check("cond_init", pthread_cond_init(&c, NULL), 0);
	check("binding timed wait", timed_wait_ms(&m, 5), ETIMEDOUT);

	__atomic_store_n(&hook_armed, 1, __ATOMIC_RELEASE);
	if ((rc = pthread_create(&t, NULL, signaller, NULL)) != 0) {
		printf("pthread-signal-unbind: pthread_create %d FAIL\n", rc);
		printf("PTHREAD_SIGNAL_UNBIND_FAIL 1\n");
		return 1;
	}
	while (!__atomic_load_n(&loaded, __ATOMIC_ACQUIRE))
		usleep(1000);
	rc = pthread_mutex_destroy(&m);
	destroy_end_ns = now_ns();
	pthread_join(t, NULL);
	__atomic_store_n(&hook_armed, 0, __ATOMIC_RELEASE);

	check("signal with the id loaded", signal_rc, 0);
	check("mutex_destroy during the signal", rc, 0);
	check("destroy returned after the signal's syscall", destroy_end_ns >= signal_end_ns, 1);
	printf("pthread-signal-unbind: destroy returned %lld us after the signal\n",
	       (long long)(destroy_end_ns - signal_end_ns) / 1000);

	check("second mutex_init", pthread_mutex_init(&m2, NULL), 0);
	check("cond reused with the second mutex", timed_wait_ms(&m2, 5), ETIMEDOUT);
	check("cond_destroy", pthread_cond_destroy(&c), 0);
	check("second mutex_destroy", pthread_mutex_destroy(&m2), 0);

	if (fails)
		printf("PTHREAD_SIGNAL_UNBIND_FAIL %d\n", fails);
	else
		printf("PTHREAD_SIGNAL_UNBIND_OK\n");
	return fails;
}
