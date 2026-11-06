/* G-v1a: a deterministic v1 capture, built against the BASE runtime (e785d843) and against the CANDIDATE runtime,
 * whose bytes must be identical.  Determinism: APP mode, one scripted thread, a callback sink into memory, the
 * timebase from the pal_host clock hook (a counter), and pthread_self fixed through -Wl,--wrap=pthread_self (the
 * thread id the THREADS chunk records).  The finalization record's scalar fields are dumped too.
 *
 * usage: v1a <variant> <out-dir>; variant: init | ex-null | ex-disabled (candidate only), each optionally prefixed
 * "unwind-": the first init attempt fails (alloc_fail), the second succeeds. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ps3dk/prof.h>

#include "prof_finalization.h"
#include "prof_test_hooks.h"
#ifdef FIXTURE_HAVE_EXT
#include "prof_spu.h"
#endif

pthread_t __wrap_pthread_self(void);
pthread_t __wrap_pthread_self(void) { return (pthread_t)0x5000; }

static uint64_t g_t = 1000;
static uint64_t clk(void) { return g_t += 10; }

static unsigned char *g_buf;
static size_t g_len, g_cap;
static long w_write(void *ctx, const void *b, unsigned long n)
{
    (void)ctx;
    if (g_len + n > g_cap) {
        g_cap = (g_len + n) * 2;
        g_buf = realloc(g_buf, g_cap);
    }
    memcpy(g_buf + g_len, b, n);
    g_len += n;
    return (long)n;
}
static int w_close(void *ctx)
{
    (void)ctx;
    return 0;
}

static int do_init(const char *v, const ps3dk_prof_config *c)
{
    ps3dk_prof_sink_callbacks cb = {w_write, NULL, w_close, NULL};
    if (ps3dk_prof_set_sink_callbacks(&cb) != PS3DK_PROF_OK)
        return -100;
    if (!strcmp(v, "init"))
        return ps3dk_prof_init(c);
#ifdef FIXTURE_HAVE_EXT
    if (!strcmp(v, "ex-null"))
        return ps3dk_prof_init_ex(c, NULL);
    if (!strcmp(v, "ex-disabled") || !strcmp(v, "ex-enabled")) {
        ps3dk_prof_ext_config x;
        memset(&x, 0, sizeof x);
        x.size = PS3DK_PROF_EXT_CONFIG_SIZE;
        x.spu_lanes_max = !strcmp(v, "ex-enabled") ? 1 : 0; /* enabled: the sensitivity control (flags 1) */
        return ps3dk_prof_init_ex(c, &x);
    }
#endif
    return -101;
}

int main(int argc, char **argv)
{
    ps3dk_prof_config c;
    const char *v;
    char path[512];
    FILE *f;
    const struct ps3dk_prof_finalization *fin;
    int rc, unwind;
    if (argc != 3)
        return 2;
    unwind = !strncmp(argv[1], "unwind-", 7);
    v = unwind ? argv[1] + 7 : argv[1];
    ps3dk_prof_test_hooks.clock = clk;
    ps3dk_prof_config_defaults(&c);
    c.mode = PS3DK_PROF_MODE_APP;
    c.app = "v1a";
    c.build_id = "fixture";
    if (unwind) {
        ps3dk_prof_test_hooks.alloc_fail = 1;
        rc = do_init(v, &c);
        ps3dk_prof_test_hooks.alloc_fail = 0;
        if (rc != PS3DK_PROF_ENOMEM) {
            printf("first init rc %d, want ENOMEM\n", rc);
            return 1;
        }
    }
    rc = do_init(v, &c);
    if (rc != PS3DK_PROF_OK) {
        printf("init rc %d\n", rc);
        return 1;
    }
    if (ps3dk_prof_thread_begin("main") != PS3DK_PROF_OK)
        return 1;
    for (int i = 0; i < 5; i++) {
        ps3dk_prof_zone z;
        ps3dk_prof_frame();
        z = PS3DK_PROF_ZONE_BEGIN("work");
        PS3DK_PROF_ZONE_END(z);
        if (i == 2 && ps3dk_prof_drain() != PS3DK_PROF_OK)
            return 1;
    }
    ps3dk_prof_thread_end();
    rc = ps3dk_prof_shutdown();
    snprintf(path, sizeof path, "%s/capture.prof", argv[2]);
    f = fopen(path, "wb");
    if (!f || fwrite(g_buf, 1, g_len, f) != g_len || fclose(f) != 0)
        return 1;
    fin = ps3dk_prof__finalization();
    snprintf(path, sizeof path, "%s/final.txt", argv[2]);
    f = fopen(path, "w");
    if (!f)
        return 1;
    fprintf(f, "shutdown %d\n", rc);
    if (fin) {
        fprintf(f, "accepted %llu result %d trailer %u io %u join_failed %u\n", (unsigned long long)fin->accepted_bytes,
                fin->shutdown_result, fin->trailer_written, fin->io_finalized, fin->join_failed);
#ifdef PS3DK_PROF_WITH_ARCS
        fprintf(f, "chunks %llu last %llu crc %08x unreg %llu session %llu freq %llu tb0 %llu\n",
                (unsigned long long)fin->completed_chunks, (unsigned long long)fin->last_chunk_sequence,
                fin->prefix_crc32, (unsigned long long)fin->unregistered_refusals,
                (unsigned long long)fin->session_id, (unsigned long long)fin->freq_hz,
                (unsigned long long)fin->tb_at_init);
#endif
    } else {
        fprintf(f, "no finalization record\n");
    }
    fclose(f);
    printf("v1a %s: %zu bytes, shutdown %d\n", argv[1], g_len, rc);
    return 0;
}
