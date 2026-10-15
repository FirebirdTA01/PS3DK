/* pthread-key-reuse: librt's pthread keys.
 *  1. A value set under a key, then the key deleted and a new key created (reusing the index): the new key starts
 *     out NULL in the thread that had the old value and in a new thread.
 *  2. Destructor iterations: a destructor that sets its key's value again is called again, at most
 *     PTHREAD_DESTRUCTOR_ITERATIONS (4) times in all, and a key whose value is NULL is not called.
 *  3. Destructors run on the exiting thread, with the value it set. */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

static int fails;
static pthread_key_t k1, k2, again_key, null_key;
static int again_calls, null_calls, value_ok = 1;
static pthread_t value_owner;

static void check(const char *what, long got, long want)
{
	printf("pthread-key-reuse: %s = %ld%s\n", what, got, got == want ? " ok" : " FAIL");
	if (got != want)
		fails++;
}

static void again_dtor(void *v)
{
	again_calls++;
	if ((intptr_t)v != again_calls || !pthread_equal(pthread_self(), value_owner))
		value_ok = 0;
	pthread_setspecific(again_key, (void *)(intptr_t)(again_calls + 1));	/* set it again: called again */
}

static void null_dtor(void *v)
{
	(void)v;
	null_calls++;
}

static void *reader(void *arg)
{
	(void)arg;
	return pthread_getspecific(k2);
}

static void *iterating(void *arg)
{
	(void)arg;
	value_owner = pthread_self();
	pthread_setspecific(again_key, (void *)(intptr_t)1);
	pthread_setspecific(null_key, NULL);
	return NULL;
}

int main(void)
{
	pthread_t t;
	void *v = (void *)1;

	check("key_create k1", pthread_key_create(&k1, NULL), 0);
	check("setspecific k1", pthread_setspecific(k1, (void *)0x1234), 0);
	check("key_delete k1", pthread_key_delete(k1), 0);
	check("key_create k2", pthread_key_create(&k2, NULL), 0);
	check("k2 reused k1's index", k2 == k1, 1);
	check("k2 starts NULL in this thread", (long)(intptr_t)pthread_getspecific(k2), 0);
	pthread_create(&t, NULL, reader, NULL);
	pthread_join(t, &v);
	check("k2 starts NULL in a new thread", (long)(intptr_t)v, 0);

	check("key_create again_key", pthread_key_create(&again_key, again_dtor), 0);
	check("key_create null_key", pthread_key_create(&null_key, null_dtor), 0);
	pthread_create(&t, NULL, iterating, NULL);
	pthread_join(t, NULL);
	check("destructor re-set its value: called 4 times", again_calls, 4);
	check("destructor saw its thread and value each time", value_ok, 1);
	check("NULL value: destructor not called", null_calls, 0);

	if (fails)
		printf("PTHREAD_KEY_REUSE_FAIL %d\n", fails);
	else
		printf("PTHREAD_KEY_REUSE_OK\n");
	return fails;
}
