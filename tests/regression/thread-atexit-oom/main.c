/* thread-atexit-oom: a thread_local destructor that cannot be registered stops the program.
 *
 * libstdc++'s __cxa_thread_atexit returns __cxa_thread_atexit_impl's result to code that ignores it, so librt
 * makes a failed registration fatal: it writes a message to stderr without allocating and aborts.  librt's
 * pthread.c is built into the row with the registration's allocation forced to fail (hook.h); the row calls
 * __cxa_thread_atexit_impl as libsupc++ would.  Expected TTY: the message, then no THREAD_ATEXIT_OOM_RETURNED. */
#include <stdio.h>

extern int __cxa_thread_atexit_impl(void (*fn)(void *), void *obj, void *dso_handle);

static void dtor(void *p) { (void)p; }

int main(void)
{
	int obj;

	printf("thread-atexit-oom: registering with the allocation failing\n");
	fflush(stdout);
	__cxa_thread_atexit_impl(dtor, &obj, NULL);
	printf("THREAD_ATEXIT_OOM_RETURNED\n");
	return 1;
}
