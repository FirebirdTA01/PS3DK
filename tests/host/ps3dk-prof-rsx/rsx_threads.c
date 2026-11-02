/* P3.3 R1 threaded runs (TSan): real producer, GPU and drainer threads over the same emulator, port and sink as
 * rsx_model.c (included with its main renamed).  Scenarios: run (zones to completion, then the seal), seal-live (the
 * seal while the producer is still issuing), detach (the producer detaches while the drainer runs), and the NEGATIVE
 * control nolock (a port whose snap lock does nothing: TSan must report the race on the published snapshot).
 * usage: rsx_threads OUTDIR SCENARIO */
#define main rsx_model_main
#include "rsx_model.c"
#undef main
#include <unistd.h>

static volatile int g_stop_gpu, g_stop_drain, g_producer_done;

static void *gpu_thread(void *a)
{
    (void)a;
    while (!__atomic_load_n(&g_stop_gpu, __ATOMIC_ACQUIRE)) {
        gpu_step(64);
        usleep(20);
    }
    return NULL;
}
static void *drain_thread(void *a)
{
    (void)a;
    while (!__atomic_load_n(&g_stop_drain, __ATOMIC_ACQUIRE)) {
        prg_drain(&D);
        usleep(50);
    }
    return NULL;
}
static int g_iters = 3000, g_detach_rc = 99;
static void *producer_thread(void *a)
{
    int detach = a != NULL;
    prg_token t[4];
    for (int i = 0; i < g_iters; i++) {
        if (B(0, &t[0]) == PRG_ESTOPPED)
            break;
        (void)M(1);
        (void)B(2, &t[1]);
        (void)E(&t[1], 2);
        (void)E(&t[0], 0);
        if (i % 64 == 0)
            usleep(10);
    }
    if (detach)
        g_detach_rc = prg_detach(&D, 5000000, NULL, NULL);
    __atomic_store_n(&g_producer_done, 1, __ATOMIC_RELEASE);
    return NULL;
}
#include <time.h>
/* threaded runs poll against real time (the deterministic traces' virtual clock would expire in a spin) */
static int real_now(void *c, uint64_t *us)
{
    struct timespec ts;
    (void)c;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    *us = (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
    return 0;
}
static int nl_lock(void *c) { (void)c; return 0; }
static int nl_unlock(void *c) { (void)c; return 0; }

int main(int argc, char **argv)
{
    pthread_t g, dr, pr;
    const char *sc;
    prg_port port = g_port;
    port.now_us = real_now;
    char name[64];
    FILE *e;
    if (argc < 3)
        return 2;
    g_dir = argv[1];
    sc = argv[2];
    pthread_mutex_init(&G.m, NULL);
    if (!strcmp(sc, "nolock")) {
        port.snap_lock = nl_lock;
        port.snap_unlock = nl_unlock;
    }
    for (int i = 0; i < g_nch; i++)
        free(g_ch[i].data);
    g_nch = 0;
    g_ntr = 0;
    gpu_reset();
    G.fail_every = 7; /* FIFO refusals interleave losses with events */
    g_led = LED_READER;
    WANT(prg_attach(&D, 7, 16, 256, 200, &port, &g_out, &g_led, g_meta, g_stage, g_scratch) == PRG_OK, "attach");
    pthread_create(&g, NULL, gpu_thread, NULL);
    pthread_create(&dr, NULL, drain_thread, NULL);
    pthread_create(&pr, NULL, producer_thread, !strcmp(sc, "detach") ? (void *)1 : NULL);
    if (!strcmp(sc, "seal-live")) {
        usleep(20000);
        prg_seal(&D); /* concurrent with the producer and the drainer */
    }
    pthread_join(pr, NULL);
    usleep(20000);
    __atomic_store_n(&g_stop_drain, 1, __ATOMIC_RELEASE);
    pthread_join(dr, NULL);
    prg_seal(&D); /* no-op if already terminal */
    __atomic_store_n(&g_stop_gpu, 1, __ATOMIC_RELEASE);
    pthread_join(g, NULL);
    gpu_step(1u << 30); /* the app's Finish */
    WANT(prg_release(&D) == PRG_OK, "%s: release once", sc);
    if (!strcmp(sc, "detach"))
        WANT(g_detach_rc == PRG_OK, "detach returned %d", g_detach_rc);
    snprintf(name, sizeof name, "r1-thr-%s", sc);
    e = finish(name);
    fprintf(e, "exit %d\nevents %llu\nlost %llu\n", D.lost_accepted || D.final_state != 1 ? 4 : 0,
            (unsigned long long)D.events_accepted, (unsigned long long)D.lost_accepted);
    fclose(e);
    printf("R1 threads %s: state %u events %llu lost %llu failures %d\n", sc, D.final_state,
           (unsigned long long)D.events_accepted, (unsigned long long)D.lost_accepted, g_bad);
    return g_bad ? 1 : 0;
}
