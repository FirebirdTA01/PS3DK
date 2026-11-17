/* Serves the real runtime's capture through the public live API for the viewer's capture-trigger end-to-end test.
 * The app code here asks for NO capture window: every window in the capture is one the viewer triggered.
 * usage: live-trigger-serve <scratch dir> <acks>
 * - Prints "PORT <n>"; waits for the first viewer (5 s); marks frames (a zone each, a drain every 10) every 2 ms
 *   until the agent has sent <acks> CAPTURE_ACKs (20 s at most), then 40 more frames; shuts down; waits up to 30 s
 *   for END; stops (1 s graceful + 1 s join); prints "SERVED ok=<0|1> stop=<rc> shutdown=<rc> acks=<n> refusals=<n>
 *   frames=<n>" and "CAPTURE <path>".
 * - Exits 0 only after one END-completed stream, every expected ACK and a clean stop. */
#define _GNU_SOURCE
#include <ps3dk/prof.h>
#include "live_api.h"
#include "prof_test_hooks.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    unsigned long want = strtoul(argv[2], NULL, 10);
    ps3dk_prof_config c;
    ps3dk_prof_config_defaults(&c);
    c.app = "e2e-trig";
    c.build_id = "e2e-trigger-build";
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
    for (int i = 0; i < 5000 && atomic_load(&a->connections) == 0; ++i)
        usleep(1000);
    if (ps3dk_prof_thread_begin("main") != PS3DK_PROF_OK)
        return 1;
    unsigned long frames = 0;
    for (int i = 0; i < 10000 && atomic_load(&a->acks) < want; ++i, ++frames) {
        PS3DK_PROF_ZONE("e2e-trig-zone");
        PS3DK_PROF_FRAME();
        if (i % 10 == 9 && ps3dk_prof_drain() != PS3DK_PROF_OK)
            return 1;
        usleep(2000);
    }
    for (int i = 0; i < 40; ++i, ++frames) {
        PS3DK_PROF_ZONE("e2e-trig-zone");
        PS3DK_PROF_FRAME();
        usleep(2000);
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
    unsigned long long acks = atomic_load(&a->acks), refusals = atomic_load(&a->ack_refusals);
    printf("SERVED ok=%d stop=%d shutdown=%d acks=%llu refusals=%llu frames=%lu\n", ok, stop, shut, acks, refusals,
           frames);
    printf("CAPTURE %s\n", rec.path);
    fflush(stdout);
    return ok && stop == PS3DK_PROF_OK && shut == PS3DK_PROF_OK && acks == want ? 0 : 1;
}
