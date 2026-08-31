/* Host reproduction of hello-ppu-pthread's wait/signal structure, on glibc.
 *
 * Question under test: is the RPCS3 hang a defect in the librt shim, or is the
 * sample's synchronization itself lost-wakeup-prone on any conforming pthreads?
 *
 * The sample uses one condition variable for two different predicates:
 * main waits for ready_workers == NWORKERS, and the workers wait for
 * release_workers. The only interleaving that matters is the one the RPCS3 log
 * showed in run-20260830-161359: exactly one worker takes the mutex before main
 * does. Forced here with sleeps, nothing else changed.
 */
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>

#define NWORKERS 2

static pthread_mutex_t shared_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shared_cond = PTHREAD_COND_INITIALIZER;

static int ready_workers;
static int release_workers;
static int completed_workers;

#ifndef USE_BROADCAST
#define WAKE(c) pthread_cond_signal(c)
#define WAKE_NAME "signal"
#else
#define WAKE(c) pthread_cond_broadcast(c)
#define WAKE_NAME "broadcast"
#endif

static void *worker_main(void *arg)
{
	long id = (long)arg;

	/* Force the interleaving the RPCS3 log showed: worker 0 takes the mutex
	 * first, main second, worker 1 last. */
	if (id == 1)
		usleep(200000);

	pthread_mutex_lock(&shared_mutex);
	ready_workers++;
	printf("worker %ld: ready_workers=%d, waking with %s\n",
	       id, ready_workers, WAKE_NAME);
	WAKE(&shared_cond);
	while (!release_workers)
		pthread_cond_wait(&shared_cond, &shared_mutex);
	completed_workers++;
	WAKE(&shared_cond);
	pthread_mutex_unlock(&shared_mutex);
	return NULL;
}

int main(void)
{
	pthread_t workers[NWORKERS];
	long i;

	printf("host repro, wake primitive = %s\n", WAKE_NAME);

	for (i = 0; i < NWORKERS; i++)
		pthread_create(&workers[i], NULL, worker_main, (void *)i);

	usleep(50000); /* Let worker 0 in first. */

	pthread_mutex_lock(&shared_mutex);
	printf("main: locked, ready_workers=%d\n", ready_workers);
	while (ready_workers < NWORKERS) {
		printf("main: waiting for ready_workers\n");
		pthread_cond_wait(&shared_cond, &shared_mutex);
		printf("main: woke, ready_workers=%d\n", ready_workers);
	}
	release_workers = 1;
	pthread_cond_broadcast(&shared_cond);
	while (completed_workers < NWORKERS)
		pthread_cond_wait(&shared_cond, &shared_mutex);
	pthread_mutex_unlock(&shared_mutex);

	for (i = 0; i < NWORKERS; i++)
		pthread_join(workers[i], NULL);

	printf("HOST_OK\n");
	return 0;
}
