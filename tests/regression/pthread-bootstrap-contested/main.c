/* pthread-bootstrap-contested: librt's pthread bootstrap (the table lock created on first use) under contention.
 *
 * The first pthread call creates librt's process-wide table lock; exactly one caller wins the claim and creates it,
 * and every other caller must wait WITHOUT starving the winner, even when the winner has a lower priority than every
 * loser.  This row builds librt's pthread.c itself with hooks (hook.h): one the winner runs right after its claim,
 * one in each pass of a loser's sleeping wait.
 *
 * Handshake: before any pthread call, a low-priority raw Lv-2 thread (priority 3000) makes the first one and wins;
 * it holds in its hook until three high-priority raw threads (priority 100), started after the claim, have each
 * reported a wait pass, and only then creates the lock.  Losers that yielded instead of sleeping would keep the
 * winner from ever running again (a yield never hands the processor to a lower priority); the row would time out.
 * Checks: all three losers reported a wait pass before the winner was released, every caller's mutex call returned
 * 0, and the mutex works afterwards. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <time.h>

enum { LOSERS = 3, WINNER_PRIORITY = 3000, LOSER_PRIORITY = 100 };
#define HOLD_LIMIT_US (10u * 1000000u)

static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
static int claimed, hold_timed_out, released;
static sys_ppu_thread_t loser_id[LOSERS];
static int loser_ids_ready;
static unsigned wait_passes[LOSERS];
static int result[1 + LOSERS] = { -1, -1, -1, -1 };

static int waiting_count(void)
{
	int n = 0;
	for (int i = 0; i < LOSERS; i++)
		n += __atomic_load_n(&wait_passes[i], __ATOMIC_ACQUIRE) != 0;
	return n;
}

/* Winner, right after its claim: hold until every loser is in the sleeping wait. */
void test_boot_claimed(void)
{
	unsigned waited = 0;

	__atomic_store_n(&claimed, 1, __ATOMIC_RELEASE);
	while (waiting_count() < LOSERS) {
		if (waited > HOLD_LIMIT_US) {
			hold_timed_out = 1;
			break;
		}
		sysUsleep(1000);
		waited += 1000;
	}
	__atomic_store_n(&released, 1, __ATOMIC_RELEASE);
}

/* A loser, in each pass of its sleeping wait. */
void test_boot_waiting(void)
{
	sys_ppu_thread_t self;

	if (!__atomic_load_n(&loser_ids_ready, __ATOMIC_ACQUIRE) || sysThreadGetId(&self) != 0)
		return;
	for (int i = 0; i < LOSERS; i++)
		if (loser_id[i] == self) {
			__atomic_add_fetch(&wait_passes[i], 1, __ATOMIC_ACQ_REL);
			return;
		}
}

/* Each thread's first pthread call: lock and unlock the statically initialised mutex. */
static void contender(void *arg)
{
	int slot = (int)(intptr_t)arg;
	int rc = pthread_mutex_lock(&m);

	if (rc == 0)
		rc = pthread_mutex_unlock(&m);
	result[slot] = rc;
	sysThreadExit(0);
}

int main(void)
{
	sys_ppu_thread_t winner;
	u64 ret;
	int fails = 0, created[LOSERS] = { 0 };

	printf("pthread-bootstrap-contested: winner priority %d, %d losers priority %d\n", WINNER_PRIORITY, LOSERS,
	       LOSER_PRIORITY);
	if (sysThreadCreate(&winner, contender, (void *)0, WINNER_PRIORITY, 0x10000, THREAD_JOINABLE,
	                    (char *)"winner") != 0) {
		printf("PTHREAD_BOOTSTRAP_CONTESTED_FAIL 1 (winner not created)\n");
		return 1;
	}
	for (unsigned waited = 0; !__atomic_load_n(&claimed, __ATOMIC_ACQUIRE); waited += 1000) {
		if (waited > HOLD_LIMIT_US) {
			/* Something bootstrapped librt's pthreads before main: the row cannot contest it. */
			printf("PTHREAD_BOOTSTRAP_CONTESTED_FAIL 1 (the winner never claimed the bootstrap)\n");
			return 1;
		}
		sysUsleep(1000);
	}
	for (int i = 0; i < LOSERS; i++) {
		if (sysThreadCreate(&loser_id[i], contender, (void *)(intptr_t)(i + 1), LOSER_PRIORITY, 0x10000,
		                    THREAD_JOINABLE, (char *)"loser") == 0)
			created[i] = 1;
		else
			printf("pthread-bootstrap-contested: loser %d not created FAIL\n", i), fails++;
	}
	__atomic_store_n(&loser_ids_ready, 1, __ATOMIC_RELEASE);
	sysThreadJoin(winner, &ret);
	for (int i = 0; i < LOSERS; i++)
		if (created[i])
			sysThreadJoin(loser_id[i], &ret);

	if (hold_timed_out || !released) {
		printf("pthread-bootstrap-contested: only %d of %d losers reached the sleeping wait FAIL\n", waiting_count(),
		       LOSERS);
		fails++;
	}
	for (int i = 0; i < LOSERS; i++)
		if (created[i])
			printf("pthread-bootstrap-contested: loser %d: %u sleeping-wait passes\n", i, wait_passes[i]);
	for (int i = 0; i < 1 + LOSERS; i++)
		if (i == 0 || created[i - 1]) {
			printf("pthread-bootstrap-contested: %s %d mutex lock/unlock = %d\n", i ? "loser" : "winner", i ? i - 1 : 0,
			       result[i]);
			if (result[i] != 0)
				fails++;
		}
	if (pthread_mutex_lock(&m) != 0 || pthread_mutex_unlock(&m) != 0) {
		printf("pthread-bootstrap-contested: mutex unusable afterwards FAIL\n");
		fails++;
	}
	if (fails)
		printf("PTHREAD_BOOTSTRAP_CONTESTED_FAIL %d\n", fails);
	else
		printf("PTHREAD_BOOTSTRAP_CONTESTED_OK\n");
	return fails;
}
