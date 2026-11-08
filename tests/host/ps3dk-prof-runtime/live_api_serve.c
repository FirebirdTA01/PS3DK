/* Serves the real runtime's own capture through the PUBLIC live API, for
 * the viewer's end-to-end test: init with the FILE sink, ps3dk_prof_live_start,
 * zones and drains that grow the file while the viewer watches, then shutdown
 * (the runtime publishes its finalization record), END, and live_stop.
 * usage: live-api-serve <scratch dir>
 * - Prints "PORT <n>"; waits for the first viewer (3 s); grows the capture in
 *   40 drains 20 ms apart; shuts down; waits up to 30 s for END to be sent;
 *   stops (1 s graceful + 1 s join); prints "SERVED ok=<0|1> stop=<rc>
 *   bytes=<n>" and "CAPTURE <path>".
 * - Exits 0 only after one END-completed stream and a clean stop. */
#define _GNU_SOURCE
#include <ps3dk/prof.h>
#include "live_api.h"
#include "prof_test_hooks.h"
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.app = "e2e-api";
    c.build_id = "e2e-api-build";
    c.max_threads = 2;
    c.max_descriptors = 16;
    c.ring_records = 8192;
    if (ps3dk_prof_set_sink_path(argv[1]) != PS3DK_PROF_OK || ps3dk_prof_init(&c) != PS3DK_PROF_OK)
        return 1;
    ps3dk_prof_test_hooks.live_ephemeral_port = 1; /* 127.0.0.1, an ephemeral port */
    ps3dk_prof_live_config lc = {.poll_us = 2000};
    if (ps3dk_prof_live_start(&lc) != PS3DK_PROF_OK)
        return 1;
    printf("PORT %u\n", (unsigned)ps3dk_prof__live_port());
    fflush(stdout);

    const live_agent *a = ps3dk_prof__live_agent();
    for (int i = 0; i < 3000 && atomic_load(&a->connections) == 0; ++i)
        usleep(1000);
    if (ps3dk_prof_thread_begin("main") != PS3DK_PROF_OK)
        return 1;
    for (int round = 0; round < 40; ++round) {
        for (int i = 0; i < 100; ++i) {
            PS3DK_PROF_ZONE("e2e-zone");
            PS3DK_PROF_FRAME();
        }
        if (ps3dk_prof_drain() != PS3DK_PROF_OK)
            return 1;
        usleep(20000);
    }
    int shut = ps3dk_prof_shutdown();

    int ok = 0;
    for (int i = 0; i < 30000 && !ok; ++i) {
        ok = atomic_load(&a->drops[LIVE_DROP_END]) >= 1;
        usleep(1000);
    }
    int stop = ps3dk_prof_live_stop(1000000, 1000000);
    ps3dk_prof_capture_record rec;
    if (ps3dk_prof_get_capture_record(&rec) != PS3DK_PROF_OK)
        return 1;
    printf("SERVED ok=%d stop=%d shutdown=%d bytes=%llu\n", ok, stop, shut,
           (unsigned long long)atomic_load(&a->bytes_delivered));
    printf("CAPTURE %s\n", rec.path);
    fflush(stdout);
    return ok && stop == PS3DK_PROF_OK && shut == PS3DK_PROF_OK ? 0 : 1;
}
