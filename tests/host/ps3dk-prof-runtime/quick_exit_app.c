/* quick_exit schedules for the profiler's at_quick_exit drain (quick-exit-test.py).
 *
 * A witness at_quick_exit handler is registered BEFORE init, so it runs AFTER the profiler's handler (handlers run
 * in reverse order of registration).  It reports, on stdout, how long the profiler's handler took, what a shutdown
 * attempted after it returns, and whether the capture file changed size.
 *   plain     THREAD mode, one zone, quick_exit: the handler drains; the witness's shutdown is EALREADY.
 *   midpass   the drain thread is parked inside a pass (3 s) when quick_exit runs: the handler gives up after its
 *             bound and returns at once; the witness sees a short elapsed time.
 *   callback  APP mode with a callback sink whose write calls quick_exit inside ps3dk_prof_drain: the handler skips
 *             (the caller is inside the drainer); the witness's shutdown is EBUSY; nothing deadlocks.
 *   after     shutdown first, then quick_exit: the handler does nothing; the file keeps its size. */
#include <ps3dk/prof.h>
#include "prof_test_hooks.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *mode;
static atomic_int armed, entered;
static struct timespec t0;
static char path[256];
static long size_before = -1;

static long file_size(void)
{
    struct stat st;
    return path[0] && stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void witness(void)
{
    struct timespec t1;
    long ms;
    int rc;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    rc = strcmp(mode, "midpass") == 0 ? 0 : ps3dk_prof_shutdown();   /* midpass: the drain thread is parked */
    printf("witness mode=%s elapsed_ms=%ld shutdown=%d size_same=%d\n", mode, ms, rc,
           size_before >= 0 && file_size() == size_before);
    fflush(stdout);
}

static void park_drainer(void)
{
    if (atomic_load(&armed) && !atomic_exchange(&entered, 1))
        sleep(3);
}

static long cb_write(void *ctx, const void *buf, unsigned long n)
{
    (void)ctx;
    (void)buf;
    if (atomic_exchange(&armed, 0)) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        quick_exit(0);
    }
    return (long)n;
}

static int cb_close(void *ctx)
{
    (void)ctx;
    return 0;
}

static ps3dk_prof_desc zdesc = { "qx-zone", "", __FILE__, (uint32_t)__LINE__, 1u, 0u, 0u };

int main(int argc, char **argv)
{
    ps3dk_prof_config c;
    ps3dk_prof_capture_record rec;
    ps3dk_prof_zone z;
    int i;
    mode = argc > 1 ? argv[1] : "plain";
    if (at_quick_exit(witness) != 0)
        return 10;
    ps3dk_prof_config_defaults(&c);
    c.mode = strcmp(mode, "callback") == 0 ? PS3DK_PROF_MODE_APP : PS3DK_PROF_MODE_THREAD;
    if (strcmp(mode, "callback") == 0) {
        ps3dk_prof_sink_callbacks cb = {cb_write, NULL, cb_close, NULL};
        if (ps3dk_prof_set_sink_callbacks(&cb) != 0)
            return 11;
    } else if (ps3dk_prof_set_sink_path(".") != 0) {
        return 11;
    }
    if (strcmp(mode, "midpass") == 0)
        ps3dk_prof_test_hooks.drainer_after_tables = park_drainer;
    if (ps3dk_prof_init(&c) != 0)
        return 12;
    if (c.mode == PS3DK_PROF_MODE_THREAD && ps3dk_prof_get_capture_record(&rec) == 0)
        memcpy(path, rec.path, sizeof path);
    if (ps3dk_prof_thread_begin("main") != 0)
        return 13;
    z = ps3dk_prof_zone_begin(&zdesc, ps3dk_prof_frame_token());
    ps3dk_prof_zone_end(&z);
    if (strcmp(mode, "callback") == 0) {
        atomic_store(&armed, 1);
        (void)ps3dk_prof_drain();   /* the write callback calls quick_exit */
        return 14;
    }
    if (strcmp(mode, "midpass") == 0) {
        atomic_store(&armed, 1);
        for (i = 0; i < 5000 && !atomic_load(&entered); i++)
            usleep(1000);
        if (!atomic_load(&entered))
            return 15;
    }
    if (strcmp(mode, "after") == 0) {
        ps3dk_prof_thread_end();
        if (ps3dk_prof_shutdown() != 0)
            return 16;
        size_before = file_size();
    }
    clock_gettime(CLOCK_MONOTONIC, &t0);
    quick_exit(0);
}
