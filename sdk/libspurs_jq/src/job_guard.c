/* C++ function-local static guards for jobs linked with
 * -mspurs-job-initialize.
 *
 * This file is linked into the job_crt.o startfile rather than the
 * runtime archive: the g++ driver searches libstdc++ before the SPURS
 * runtime archives, so guards defined in an archive lost to libsupc++'s
 * guard.o and then clashed with it when the rest of the runtime object
 * was pulled in.  A startfile is linked before any archive is searched,
 * so these definitions always win and guard.o is never pulled.
 */
#include <stdint.h>

/* Function-local statics: a job runs on one SPU and nothing else touches
   its local store, so the guards need no locking.  Defining them here
   also keeps libsupc++'s versions, and the exception, terminate and
   demangler code they bring, out of job images. */
int __cxa_guard_acquire(uint64_t *guard)
{
	return !*(volatile uint8_t *)guard;
}

void __cxa_guard_release(uint64_t *guard)
{
	*(volatile uint8_t *)guard = 1;
}

void __cxa_guard_abort(uint64_t *guard)
{
	(void)guard;
}
