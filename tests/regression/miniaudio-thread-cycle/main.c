/* Regression row: miniaudio's device worker thread created, run and joined through our librt pthreads, twice
 * (t_e87d59d7; the probe is in probe.c).  The row passes only on the probe's PROBE_OK, reported here as
 * MINIAUDIO_THREAD_CYCLE_OK with the ABI; anything else is MINIAUDIO_THREAD_CYCLE_FAIL.  A join that never returns
 * hangs the probe, and the boot's timeout reports it.
 */
#include <stdio.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

#ifdef __LP64__
#define ABI "lp64"
#else
#define ABI "ilp32"
#endif

int miniaudio_thread_cycle_probe(void);

int main(void)
{
    int rc = miniaudio_thread_cycle_probe();
    printf("%s " ABI "\n", rc == 0 ? "MINIAUDIO_THREAD_CYCLE_OK" : "MINIAUDIO_THREAD_CYCLE_FAIL");
    fflush(stdout);
    return rc;
}
