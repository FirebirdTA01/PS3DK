/* Host driver for the counter-mutex lock failures (counters-test.py).  Built with -DPS3DK_PROF_WITH_COUNTERS and
 * -DPS3DK_PROF_TEST_HOOKS.  argv: SITE SKIP OUT.  The drainer's counter lock fails once at SITE (the
 * counter_lock_fail_site hook; 0 = never) after SKIP locks there.  The program records counters around two manual
 * drains and a shutdown, writes the capture to OUT and prints the three results; counters-test.py checks them and
 * the capture.  Exit 0 unless the program itself could not run. */
#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include <ps3dk/prof_counter.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char bytes[1 << 20];
static size_t used;
static long put(void *ctx, const void *p, unsigned long n)
{
    (void)ctx;
    if (n > sizeof bytes - used)
        return -1;
    memcpy(bytes + used, p, n);
    used += n;
    return (long)n;
}
static int close_sink(void *ctx) { (void)ctx; return 0; }

int main(int argc, char **argv)
{
    ps3dk_prof_config cfg;
    ps3dk_prof_sink_callbacks sink = {put, NULL, close_sink, NULL};
    int d1, d2, sh;
    FILE *f;

    if (argc != 4)
        return 2;
    ps3dk_prof_config_defaults(&cfg);
    cfg.app = "counters-lock";
    cfg.max_threads = 4;
    cfg.max_descriptors = 16;
    cfg.ring_records = 512;
    cfg.quiesce_timeout_us = 0;
    if (ps3dk_prof_set_sink_callbacks(&sink) != 0 || ps3dk_prof_init(&cfg) != 0 ||
        ps3dk_prof_thread_begin("main") != 0)
        return 3;
    ps3dk_prof_test_hooks.counter_lock_fail_site = (uint32_t)strtoul(argv[1], NULL, 0);
    ps3dk_prof_test_hooks.counter_lock_fail_skip = (uint32_t)strtoul(argv[2], NULL, 0);

    /* Every drain has new names and samples, so each one takes the copy and commit locks. */
    (void)PS3DK_PROF_COUNTER("a.first", 1);
    (void)PS3DK_PROF_COUNTER("a.second", 2);
    ps3dk_prof_frame();
    (void)PS3DK_PROF_COUNTER("a.first", 3);
    d1 = ps3dk_prof_drain();
    (void)PS3DK_PROF_COUNTER("a.first", 4);
    (void)PS3DK_PROF_COUNTER("b.third", 5);
    ps3dk_prof_frame();
    d2 = ps3dk_prof_drain();
    (void)PS3DK_PROF_COUNTER("a.first", 6);
    ps3dk_prof_thread_end();
    sh = ps3dk_prof_shutdown();

    f = fopen(argv[3], "wb");
    if (!f || fwrite(bytes, 1, used, f) != used || fclose(f) != 0)
        return 4;
    printf("drain1=%d drain2=%d shutdown=%d bytes=%zu\n", d1, d2, sh, used);
    return 0;
}
