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

static volatile ma_uint32 probeCallbacks = 0;
static volatile ma_uint32 probeFrames = 0;
static volatile int probeWorkerSeen = 0;
static pthread_t probeWorker;

/* Runs on miniaudio's device worker thread: counts the call and records the thread it ran on. */
static void probeData(ma_device* device, void* output, const void* input, ma_uint32 frameCount)
{
    (void)input;
    ma_silence_pcm_frames(output, frameCount, device->playback.format, device->playback.channels);
    if (!probeWorkerSeen)
    {
        probeWorker = pthread_self();
        probeWorkerSeen = 1;
    }
    probeCallbacks += 1;
    probeFrames += frameCount;
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
    ma_uint32 atStop;
    int waitedMilliseconds = 0;
    char reason[96];

    probeCallbacks = 0;
    probeFrames = 0;
    probeWorkerSeen = 0;
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
    /* Up to two seconds for twenty callbacks from the worker (the null backend's period is about 10 ms). */
    while (waitedMilliseconds < 2000 && probeCallbacks < 20)
    {
        ma_sleep(10);
        waitedMilliseconds += 10;
    }
    printf("MAPROBE cycle %d callbacks=%u frames=%u after %d ms; on a thread other than main's: %s\n", cycle,
           (unsigned)probeCallbacks, (unsigned)probeFrames, waitedMilliseconds,
           probeWorkerSeen ? (pthread_equal(probeWorker, pthread_self()) ? "no" : "yes") : "none ran");
    fflush(stdout);
    result = ma_device_stop(&device);
    atStop = probeCallbacks;
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
    printf("MAPROBE cycle %d device_uninit returned (worker joined); callbacks after stop: %u\n", cycle,
           (unsigned)(probeCallbacks - atStop));
    fflush(stdout);
    if (probeCallbacks == 0)
    {
        snprintf(reason, sizeof(reason), "cycle %d no data callback in %d ms", cycle, waitedMilliseconds);
        return probeFail(reason);
    }
    if (!probeWorkerSeen || pthread_equal(probeWorker, pthread_self()))
    {
        snprintf(reason, sizeof(reason), "cycle %d the data callback ran on main's thread", cycle);
        return probeFail(reason);
    }
    if (probeCallbacks != atStop)
    {
        snprintf(reason, sizeof(reason), "cycle %d %u callbacks after ma_device_stop", cycle, (unsigned)(probeCallbacks - atStop));
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
