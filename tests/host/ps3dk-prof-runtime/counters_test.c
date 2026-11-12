/* Host driver for named per-frame counters (counters-test.py).  The same program is built twice: with
 * -DPS3DK_PROF_WITH_COUNTERS (the counters runtime) and without it (the default runtime).  Both record the same
 * zones on two threads, named like an application's wait/drain zones (D4: they must decode identically in both
 * builds); the counters build also records counters and every refusal kind.  argv[1] = output capture path. */
#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#ifdef PS3DK_PROF_WITH_COUNTERS
#include <ps3dk/prof_counter.h>
#endif

static unsigned checks, failed;
#define CHECK(x) do { ++checks; if (!(x)) { ++failed; fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); } } while (0)
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

static void zones(int frames)
{
    for (int f = 0; f < frames; ++f) {
        ps3dk_prof_zone w = PS3DK_PROF_ZONE_BEGIN("wait.rsx_label");
        {
            ps3dk_prof_zone d = PS3DK_PROF_ZONE_BEGIN("drain.retired");
            PS3DK_PROF_ZONE_END(d);
        }
        PS3DK_PROF_ZONE_END(w);
#ifdef PS3DK_PROF_WITH_COUNTERS
        CHECK(PS3DK_PROF_COUNTER("arena.bytes_used", 4096 * (f + 1)) == PS3DK_PROF_OK);
        CHECK(PS3DK_PROF_COUNTER("arena.fallbacks", -f) == PS3DK_PROF_OK);
#endif
        ps3dk_prof_frame();
    }
}

static void *worker(void *arg)
{
    (void)arg;
#ifdef PS3DK_PROF_WITH_COUNTERS
    CHECK(ps3dk_prof_counter("worker.before_register", 1) == PS3DK_PROF_EPERM); /* refused: no incarnation */
#endif
    CHECK(ps3dk_prof_thread_begin("worker") == 0);
    {
        ps3dk_prof_zone w = PS3DK_PROF_ZONE_BEGIN("wait.spu_job");
        PS3DK_PROF_ZONE_END(w);
    }
#ifdef PS3DK_PROF_WITH_COUNTERS
    CHECK(ps3dk_prof_counter("worker.jobs", 7) == PS3DK_PROF_OK); /* frame marker 0: this thread never frames */
#endif
    ps3dk_prof_thread_end();
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    ps3dk_prof_config cfg;
    ps3dk_prof_config_defaults(&cfg);
    cfg.app = "counters";
    cfg.max_threads = 4;
    cfg.max_descriptors = 16;
    cfg.ring_records = 512;
    cfg.quiesce_timeout_us = 0;
    ps3dk_prof_sink_callbacks sink = {put, NULL, close_sink, NULL};
    CHECK(ps3dk_prof_set_sink_callbacks(&sink) == 0);
#ifdef PS3DK_PROF_WITH_COUNTERS
    CHECK(ps3dk_prof_counter("before.init", 1) == PS3DK_PROF_ENOTINIT);
#endif
    CHECK(ps3dk_prof_init(&cfg) == 0);
    CHECK(ps3dk_prof_thread_begin("main") == 0);
    zones(2);
    CHECK(ps3dk_prof_drain() == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, worker, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0);
    zones(2);
#ifdef PS3DK_PROF_WITH_COUNTERS
    CHECK(ps3dk_prof_counter("", 1) == PS3DK_PROF_EINVAL);
    CHECK(ps3dk_prof_counter("has space", 1) == PS3DK_PROF_EINVAL);
    {
        char name[16];
        int full = 0;
        for (int i = 0; i < 70; ++i) { /* 3 names so far; 61 more fit, the other 9 are refused */
            snprintf(name, sizeof name, "n%02d", i);
            int rc = ps3dk_prof_counter(name, i);
            CHECK(rc == PS3DK_PROF_OK || rc == PS3DK_PROF_EFULL);
            full += rc == PS3DK_PROF_EFULL;
        }
        CHECK(full == 9);
    }
#endif
    CHECK(ps3dk_prof_drain() == 0);
    ps3dk_prof_thread_end();
    int rc = ps3dk_prof_shutdown();
    CHECK(rc == PS3DK_PROF_OK);
#ifdef PS3DK_PROF_WITH_COUNTERS
    CHECK(ps3dk_prof_counter("after.shutdown", 1) == PS3DK_PROF_ENOTINIT ||
          ps3dk_prof_counter("after.shutdown", 1) == PS3DK_PROF_ESTOPPED);
#endif
    FILE *f = fopen(argv[1], "wb");
    CHECK(f && fwrite(bytes, 1, used, f) == used && fclose(f) == 0);
    printf("counters-test-driver: checks=%u failed=%u bytes=%zu shutdown=%d\n", checks, failed, used, rc);
    return failed ? 1 : 0;
}
