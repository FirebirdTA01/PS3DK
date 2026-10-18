/* Regression row miniaudio-thread-cycle (t_e87d59d7): miniaudio's real thread path on the PPU.
 *
 * miniaudio runs as released (0.11.25, fetched and hash-checked by CMakeLists.txt): ma_device_init creates the
 * device's worker thread (and the null backend's own) through ma_thread_create, i.e. pthreads over our librt; the
 * null backend drives the data callback from that worker, and ma_device_uninit stops and joins both.
 *
 * Hard to pass by accident: two full init/start/stop/uninit cycles, so threads are created and joined twice; each
 * cycle needs callbacks, on a thread other than main's, and none after the stop; every returned code is checked.
 * The last line is PROBE_OK or PROBE_FAIL <reason>, and the return value matches it (0 for PROBE_OK).
 *
 * Based on the EMP Engine team's t_e87d59d7 probe (2026-10-18), which they offered for our suite.
 */
#define MA_NO_RUNTIME_LINKING
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_NULL
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <pthread.h>
#include <stdio.h>

/* Shared with the worker through __atomic builtins: the counters are atomic, and probeWorker is written once by the
   worker and published by the release store of probeWorkerSeen (read with acquire), so main reads a complete value. */
static ma_uint32 probeCallbacks = 0;
static ma_uint32 probeFrames = 0;
static int probeWorkerSeen = 0;
static pthread_t probeWorker;
#ifndef PROBE_CALLBACKS_WANTED
#define PROBE_CALLBACKS_WANTED 20   /* a negative-control build raises it past what 2 s can deliver: the row must FAIL */
#endif

/* Runs on miniaudio's device worker thread: counts the call and records the thread it ran on. */
static void probeData(ma_device* device, void* output, const void* input, ma_uint32 frameCount)
{
    (void)input;
    ma_silence_pcm_frames(output, frameCount, device->playback.format, device->playback.channels);
    if (!__atomic_load_n(&probeWorkerSeen, __ATOMIC_ACQUIRE))
    {
        probeWorker = pthread_self();
        __atomic_store_n(&probeWorkerSeen, 1, __ATOMIC_RELEASE);
    }
    __atomic_fetch_add(&probeFrames, frameCount, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&probeCallbacks, 1, __ATOMIC_SEQ_CST);
}

static int probeFail(const char* reason)
{
    printf("PROBE_FAIL %s\n", reason);
    fflush(stdout);
    return 1;
}

/* One full device cycle; 0 when it passed, otherwise PROBE_FAIL has been printed. */
static int probeCycle(ma_context* context, int cycle)
{
    ma_device_config config;
    ma_device device;
    ma_result result;
    ma_uint32 atStop, callbacks, frames, finalCallbacks;
    int waitedMilliseconds = 0, seen, onMain;
    char reason[96];

    /* no worker exists yet: plain resets, published to the worker by the thread creation in ma_device_init */
    __atomic_store_n(&probeCallbacks, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&probeFrames, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&probeWorkerSeen, 0, __ATOMIC_SEQ_CST);
    config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 2;
    config.sampleRate = 48000;
    config.dataCallback = probeData;
    result = ma_device_init(context, &config, &device);
    printf("MAPROBE cycle %d device_init=%d (worker thread created)\n", cycle, (int)result);
    fflush(stdout);
    if (result != MA_SUCCESS)
    {
        snprintf(reason, sizeof(reason), "cycle %d ma_device_init=%d", cycle, (int)result);
        return probeFail(reason);
    }
    result = ma_device_start(&device);
    printf("MAPROBE cycle %d device_start=%d\n", cycle, (int)result);
    fflush(stdout);
    if (result != MA_SUCCESS)
    {
        ma_device_uninit(&device);
        snprintf(reason, sizeof(reason), "cycle %d ma_device_start=%d", cycle, (int)result);
        return probeFail(reason);
    }
    /* Up to two seconds for PROBE_CALLBACKS_WANTED callbacks from the worker (the null backend's period is about
       10 ms); the same count is required below. */
    while (waitedMilliseconds < 2000 && __atomic_load_n(&probeCallbacks, __ATOMIC_SEQ_CST) < PROBE_CALLBACKS_WANTED)
    {
        ma_sleep(10);
        waitedMilliseconds += 10;
    }
    /* one stable snapshot for the report and the checks */
    callbacks = __atomic_load_n(&probeCallbacks, __ATOMIC_SEQ_CST);
    frames = __atomic_load_n(&probeFrames, __ATOMIC_SEQ_CST);
    seen = __atomic_load_n(&probeWorkerSeen, __ATOMIC_ACQUIRE);   /* pairs with the worker's release store */
    onMain = seen && pthread_equal(probeWorker, pthread_self());
    printf("MAPROBE cycle %d callbacks=%u frames=%u after %d ms; on a thread other than main's: %s\n", cycle,
           (unsigned)callbacks, (unsigned)frames, waitedMilliseconds, seen ? (onMain ? "no" : "yes") : "none ran");
    fflush(stdout);
    result = ma_device_stop(&device);
    atStop = __atomic_load_n(&probeCallbacks, __ATOMIC_SEQ_CST);
    printf("MAPROBE cycle %d device_stop=%d\n", cycle, (int)result);
    fflush(stdout);
    if (result != MA_SUCCESS)
    {
        ma_device_uninit(&device);
        snprintf(reason, sizeof(reason), "cycle %d ma_device_stop=%d", cycle, (int)result);
        return probeFail(reason);
    }
    ma_sleep(50);
    /* ma_device_uninit returns nothing (miniaudio 0.11): it signals the worker and joins it (ma_thread_wait). A join
       that never returns hangs here, and the run's watchdog reports it. */
    ma_device_uninit(&device);
    finalCallbacks = __atomic_load_n(&probeCallbacks, __ATOMIC_SEQ_CST);   /* the worker is joined: no more writes */
    printf("MAPROBE cycle %d device_uninit returned (worker joined); callbacks after stop: %u\n", cycle,
           (unsigned)(finalCallbacks - atStop));
    fflush(stdout);
    if (callbacks < PROBE_CALLBACKS_WANTED)
    {
        snprintf(reason, sizeof(reason), "cycle %d only %u of %d data callbacks in %d ms", cycle, (unsigned)callbacks,
                 PROBE_CALLBACKS_WANTED, waitedMilliseconds);
        return probeFail(reason);
    }
    if (!seen || onMain)
    {
        snprintf(reason, sizeof(reason), "cycle %d the data callback ran on main's thread", cycle);
        return probeFail(reason);
    }
    if (finalCallbacks != atStop)
    {
        snprintf(reason, sizeof(reason), "cycle %d %u callbacks after ma_device_stop", cycle, (unsigned)(finalCallbacks - atStop));
        return probeFail(reason);
    }
    return 0;
}

/* 0 and PROBE_OK when both cycles passed; 1 and PROBE_FAIL <reason> otherwise. */
int miniaudio_thread_cycle_probe(void)
{
    ma_backend backends[] = { ma_backend_null };
    ma_context context;
    ma_result result;
    char reason[64];
    int cycle;

    result = ma_context_init(backends, 1, NULL, &context);
    printf("MAPROBE context_init=%d\n", (int)result);
    fflush(stdout);
    if (result != MA_SUCCESS)
    {
        snprintf(reason, sizeof(reason), "ma_context_init=%d", (int)result);
        return probeFail(reason);
    }
    /* Two cycles: a first worker leaked or left unjoined shows in the second. */
    for (cycle = 1; cycle <= 2; ++cycle)
    {
        if (probeCycle(&context, cycle) != 0)
        {
            ma_context_uninit(&context);
            return 1;
        }
    }
    result = ma_context_uninit(&context);
    if (result != MA_SUCCESS)
    {
        snprintf(reason, sizeof(reason), "ma_context_uninit=%d", (int)result);
        return probeFail(reason);
    }
    printf("PROBE_OK\n");
    fflush(stdout);
    return 0;
}
