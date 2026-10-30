/* ps3dk-debug-wait.c -- compiled into a target by ps3_add_debug_rpcs3().
 *
 * RPCS3 starts a game booted from its command line at once, so a short
 * program can finish before GDB connects. When the debug launcher boots the
 * program it first creates the marker file ps3dk-debug-wait next to the SELF
 * (RPCS3 mounts that directory as /app_home). This constructor runs before
 * main and, only if the marker exists, waits until GDB sets
 * __ps3dk_debug_go (the generated .gdbinit does), for at most two minutes.
 * Without the marker it returns at once: normal runs and runs on hardware
 * are unaffected. No code in the application is needed.
 */
#include <sys/stat.h>
#include <sys/timer.h>

/* Written by GDB through the generated .gdbinit; volatile so the wait loop
   reloads it. Kept as an ordinary global so GDB finds it by name. */
volatile int __ps3dk_debug_go = 0;

__attribute__((constructor(101), used)) static void ps3dk_debug_wait(void)
{
    struct stat st;
    if (stat("/app_home/ps3dk-debug-wait", &st) != 0)
        return;
    for (unsigned waited_ms = 0; !__ps3dk_debug_go && waited_ms < 120000u; waited_ms += 10u)
        sys_timer_usleep(10000);
}
