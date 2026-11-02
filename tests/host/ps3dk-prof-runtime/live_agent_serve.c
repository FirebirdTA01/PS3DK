/* Serves one capture through the real live agent, for the viewer's
 * end-to-end test: grows a copy of <capture> in chunks while the agent
 * serves it, then publishes a finalization record, as the runtime will.
 * usage: live-agent-serve <capture.prof> <scratch dir> [chunk delay ms]
 * Prints "PORT <n>" once listening; exits 0 after one completed stream
 * (END sent), 1 on failure or after 30 s. */
#define _GNU_SOURCE
#include "live_agent.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef PS3DK_PROF_TEST_HOOKS
#include "prof_test_hooks.h"
ps3dk_prof_test_hooks_t ps3dk_prof_test_hooks;
#endif

static _Atomic(const struct ps3dk_prof_finalization *) g_fin;
static struct ps3dk_prof_finalization g_rec;

static const struct ps3dk_prof_finalization *fin(void *ctx)
{
    (void)ctx;
    return atomic_load(&g_fin);
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4)
        return 2;
    unsigned delay_ms = argc == 4 ? (unsigned)strtoul(argv[3], NULL, 10) : 20u;
    FILE *in = fopen(argv[1], "rb");
    if (!in)
        return 2;
    static unsigned char buf[1 << 24];
    size_t len = fread(buf, 1, sizeof buf, in);
    fclose(in);
    char path[1024];
    snprintf(path, sizeof path, "%s/served.prof", argv[2]);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        return 2;

    static live_agent agent;
    live_config cfg;
    live_config_defaults(&cfg);
    cfg.port = 0;
    cfg.loopback_only = argc == 4 ? 0 : 1; /* the slow (screenshot) mode serves the host too */
    cfg.poll_us = 2000;
    live_source src = {.path = path, .session_id = 0x5e55, .app = "e2e", .app_len = 3,
                       .build_id = "e2e-build", .build_id_len = 9, .finalization = fin};
    if (live_agent_start(&agent, &cfg, &src) != LIVE_OK)
        return 1;
    printf("PORT %u\n", (unsigned)live_agent_port(&agent));
    fflush(stdout);

    /* Wait for the viewer, then grow the file while it watches. */
    int connect_wait_ms = argc == 4 ? 30000 : 3000;
    for (int i = 0; i < connect_wait_ms && atomic_load(&agent.connections) == 0; ++i)
        pal_sleep_us(1000);
    size_t step = len / 40 + 1;
    for (size_t off = 0; off < len; off += step) {
        size_t n = len - off < step ? len - off : step;
        if (write(fd, buf + off, n) != (ssize_t)n)
            return 1;
        pal_sleep_us((uint64_t)delay_ms * 1000);
    }
    if (close(fd) != 0)
        return 1;
    g_rec = (struct ps3dk_prof_finalization){.accepted_bytes = len, .io_finalized = 1, .trailer_written = 1};
    atomic_store(&g_fin, &g_rec);

    int ok = 0;
    for (int i = 0; i < 30000 && !ok; ++i) {
        ok = atomic_load(&agent.drops[LIVE_DROP_END]) >= 1;
        pal_sleep_us(1000);
    }
    int stop = live_agent_stop(&agent, 1000000, 1000000);
    printf("SERVED ok=%d stop=%d bytes=%llu\n", ok, stop, (unsigned long long)atomic_load(&agent.bytes_delivered));
    return ok && stop == LIVE_OK ? 0 : 1;
}
