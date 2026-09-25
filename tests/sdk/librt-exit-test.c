#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
static jmp_buf terminated;
static unsigned finalized;
static int status;
static void (*registered)(void);
static int registration_error;
static int capture_atexit(void (*fn)(void))
{
    registered = fn;
    return registration_error;
}
#define _fini fixture_fini
#define atexit capture_atexit
#include EXIT_SOURCE
#undef atexit
#undef _fini
void fixture_fini(void) { ++finalized; }
void sysProcessExit(int code) { status = code; longjmp(terminated, 1); }
int main(void)
{
    if (!setjmp(terminated)) __librt_exit(37);
    if (status != 37 || finalized) {
        fprintf(stderr, "FAIL: low-level exit finalized=%u status=%d\n", finalized, status);
        return 1;
    }
#ifdef HAS_REGISTER_FINI
    __librt_register_fini();
    if (registered != fixture_fini || finalized) return 1;
    /* Normal exit invokes the registered callback. */
    registered();
    if (finalized != 1) return 1;
    registration_error = 1;
    if (!setjmp(terminated)) {
        __librt_register_fini();
        fprintf(stderr, "FAIL: atexit registration failure returned\n");
        return 1;
    }
    if (status != 1 || finalized != 1) return 1;
#endif
    puts("librt-exit: PASS");
    return 0;
}
