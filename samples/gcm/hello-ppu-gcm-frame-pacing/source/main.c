/* Three finite presentation phases. Callback timing is an observation, not a
 * refresh-rate or hardware dropped-frame counter. No shaders or input devices. */
#include <cell/gcm.h>
#include <cell/sysutil.h>
#include <sysutil/video.h>
#include <sys/sys_time.h>
#include <sys/process.h>
#include <stdatomic.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>

SYS_PROCESS_PARAM(1001, 0x100000);
enum { FRAMES = 120, PHASES = 3, STAMPS = PHASES * (FRAMES + 1) + 8 };
enum { FIFO_BYTES = 1 << 20, HOST_BYTES = 32 << 20 };
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "callbacks need lock-free unsigned atomics");
static atomic_uint flip_count, callback_overflow, quit_requested;
static uint64_t flip_time[STAMPS];
static uint32_t width, height, color_pitch, depth_pitch;
static uint32_t color_offset[2], depth_offset;
static unsigned buffer_id = 1;
static int failed;

static uint64_t now_us(void) { return (uint64_t)sys_time_get_system_time(); }

static void on_flip(uint32_t head)
{
    (void)head;
    /* There is only one outstanding flip. Publish its timestamp last; the PPU
     * does not submit the next flip until it acquires this completion count. */
    unsigned n = atomic_load_explicit(&flip_count, memory_order_relaxed);
    if (n >= STAMPS) {
        atomic_store_explicit(&callback_overflow, 1, memory_order_release);
        return;
    }
    flip_time[n] = now_us();
    atomic_store_explicit(&flip_count, n + 1, memory_order_release);
}

static void on_sysutil(uint64_t status, uint64_t param, void *data)
{
    (void)param; (void)data;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        atomic_store_explicit(&quit_requested, 1, memory_order_release);
}

static int callbacks(void)
{
    int rc = cellSysutilCheckCallback();
    if (rc) { printf("GCM_PACING_FAIL sysutil rc=%08x\n", (unsigned)rc); failed = 1; }
    return !failed;
}

static void target(CellGcmContextData *ctx)
{
    CellGcmSurface s = {0};
    s.colorFormat = GCM_SURFACE_X8R8G8B8;
    s.colorTarget = GCM_SURFACE_TARGET_0;
    for (unsigned i = 0; i < 4; ++i) {
        s.colorLocation[i] = GCM_LOCATION_RSX;
        s.colorPitch[i] = i ? 64 : color_pitch;
    }
    s.colorOffset[0] = color_offset[buffer_id];
    s.depthFormat = GCM_SURFACE_ZETA_Z16;
    s.depthLocation = GCM_LOCATION_RSX;
    s.depthOffset = depth_offset; s.depthPitch = depth_pitch;
    s.type = GCM_SURFACE_TYPE_LINEAR; s.antiAlias = GCM_SURFACE_CENTER_1;
    s.width = width; s.height = height;
    cellGcmSetSurface(ctx, &s);
    cellGcmSetColorMask(ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
    cellGcmSetColorMaskMrt(ctx, 0);
}

static void rectangle(CellGcmContextData *ctx, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t argb)
{
    cellGcmSetScissor(ctx, x, y, w, h);
    cellGcmSetClearColor(ctx, argb);
    cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
}

static void render(CellGcmContextData *ctx, unsigned phase, unsigned frame)
{
    static const uint32_t colors[PHASES] = {0xff30c080, 0xffffa040, 0xff5080ff};
    target(ctx);
    rectangle(ctx, 0, 0, width, height, 0xff101824);
    uint32_t stripe = width / 16;
    uint32_t x = (frame * 9u) % (width - stripe);
    rectangle(ctx, x, 32, stripe, height - 64, colors[phase]);
    /* One, two or three top bars identify the phase without font/shader assets. */
    for (unsigned n = 0; n <= phase; ++n)
        rectangle(ctx, 16 + n * 80, 8, 64, 12, colors[phase]);
    rectangle(ctx, 16, height - 16, (width - 32) * (frame + 1) / FRAMES, 8, colors[phase]);
}

static int present(CellGcmContextData *ctx)
{
    unsigned before = atomic_load_explicit(&flip_count, memory_order_acquire);
    cellGcmResetFlipStatus();
    int rc = cellGcmSetFlip(ctx, (uint8_t)buffer_id);
    if (rc) { printf("GCM_PACING_FAIL submit rc=%08x\n", (unsigned)rc); return 0; }
    cellGcmFlush(ctx);
    uint64_t start = now_us();
    for (;;) {
        unsigned after = atomic_load_explicit(&flip_count, memory_order_acquire);
        if (atomic_load_explicit(&callback_overflow, memory_order_acquire) || after > before + 1) {
            puts("GCM_PACING_FAIL unexpected callback count"); return 0;
        }
        if (after == before + 1 && cellGcmGetFlipStatus() == 0) break;
        /* Quit is recorded here, but the one in-flight flip is drained before
         * leaving. Failure and this wait remain bounded even on a stalled RSX. */
        if (!callbacks()) return 0;
        if (now_us() - start >= 2000000) {
            printf("GCM_PACING_FAIL flip timeout callbacks=%u status=%u\n",
                   after - before, cellGcmGetFlipStatus());
            return 0;
        }
        usleep(500);
    }
    buffer_id ^= 1u;
    return 1;
}

static int display_init(void)
{
    videoState state;
    videoResolution resolution;
    videoConfiguration video = {0};
    if (videoGetState(0, 0, &state) || state.state != 0 ||
        videoGetResolution(state.displayMode.resolution, &resolution)) return 0;
    width = resolution.width; height = resolution.height;
    if (width < 320 || height < 240 || width > 4096 || height > 4096) return 0;
    color_pitch = (width * 4 + 63) & ~63u;
    depth_pitch = (width * 2 + 63) & ~63u;
    video.resolution = state.displayMode.resolution; video.format = VIDEO_BUFFER_FORMAT_XRGB;
    video.aspect = state.displayMode.aspect; video.pitch = color_pitch;
    if (videoConfigure(0, &video, NULL, 0)) return 0;
    CellGcmConfig config;
    cellGcmGetConfiguration(&config);
    uint64_t color_size = (uint64_t)color_pitch * height;
    uint64_t total = 2 * color_size + (uint64_t)depth_pitch * height;
    if (!config.localAddress || total > config.localSize) return 0;
    for (unsigned n = 0; n < 2; ++n) {
        void *ptr = (void *)((uintptr_t)config.localAddress + (uintptr_t)(n * color_size));
        if (cellGcmAddressToOffset(ptr, &color_offset[n]) ||
            cellGcmSetDisplayBuffer((uint8_t)n, color_offset[n], color_pitch, width, height)) return 0;
    }
    void *depth = (void *)((uintptr_t)config.localAddress + (uintptr_t)(2 * color_size));
    if (cellGcmAddressToOffset(depth, &depth_offset)) return 0;
    printf("GCM_PACING_DISPLAY width=%u height=%u pitch=%u local_bytes=%" PRIu64 "\n",
           width, height, color_pitch, total);
    return 1;
}

static int summary(const char *name, unsigned first, unsigned frames, uint64_t missed, uint64_t period)
{
    unsigned end = atomic_load_explicit(&flip_count, memory_order_acquire);
    if (end - first != frames || frames < 2) return 0;
    uint64_t intervals[FRAMES - 1];
    for (unsigned i = 1; i < frames; ++i) {
        if (flip_time[first + i] < flip_time[first + i - 1]) return 0;
        intervals[i - 1] = flip_time[first + i] - flip_time[first + i - 1];
    }
    for (unsigned i = 1; i < frames - 1; ++i) {
        uint64_t value = intervals[i]; unsigned j = i;
        while (j && intervals[j - 1] > value) { intervals[j] = intervals[j - 1]; --j; }
        intervals[j] = value;
    }
    printf("GCM_PACING_PHASE name=%s submitted=%u flips_observed=%u app_period_us=%" PRIu64
           " application_deadlines_missed=%" PRIu64 " min_us=%" PRIu64 " median_us=%" PRIu64
           " max_us=%" PRIu64 "\n", name, frames, end - first, period, missed,
           intervals[0], intervals[(frames - 1) / 2], intervals[frames - 2]);
    return 1;
}

int main(void)
{
    static const char *names[PHASES] = {"vsync", "hsync", "paced-vsync"};
    void *host = memalign(1 << 20, HOST_BYTES);
    if (!host || cellGcmInit(FIFO_BYTES, HOST_BYTES, host) || !display_init()) {
        puts("GCM_PACING_FAIL initialization"); return 1;
    }
    /* Keep mapped FIFO storage alive until process teardown. Never rewind its
     * cursor with gcmSetDefaultFifoSize after cellGcmInit. */
    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    if (cellSysutilRegisterCallback(0, on_sysutil, NULL)) {
        puts("GCM_PACING_FAIL callback registration"); return 1;
    }
    cellGcmSetFlipHandler(on_flip);
    unsigned completed = 0;
    for (unsigned phase = 0; phase < PHASES && !failed; ++phase) {
        if (!callbacks() || atomic_load_explicit(&quit_requested, memory_order_acquire)) break;
        cellGcmSetFlipMode(phase == 1 ? GCM_FLIP_HSYNC : GCM_FLIP_VSYNC);
        printf("GCM_PACING_BEGIN name=%s bars=%u budget=%u\n", names[phase], phase + 1, FRAMES);
        unsigned first = atomic_load_explicit(&flip_count, memory_order_acquire), frame = 0;
        uint64_t period = phase == 2 ? 33333 : 0, deadline = now_us(), missed = 0;
        for (; frame < FRAMES; ++frame) {
            if (!callbacks() || atomic_load_explicit(&quit_requested, memory_order_acquire)) break;
            if (period) {
                while (now_us() < deadline) {
                    if (!callbacks() || atomic_load_explicit(&quit_requested, memory_order_acquire)) break;
                    usleep(500);
                }
                if (failed || atomic_load_explicit(&quit_requested, memory_order_acquire)) break;
                uint64_t late = now_us() - deadline;
                uint64_t skipped = late / period;
                missed += skipped; deadline += (skipped + 1) * period;
            }
            render(ctx, phase, frame);
            if (!present(ctx)) { failed = 1; break; }
        }
        if (failed || atomic_load_explicit(&quit_requested, memory_order_acquire)) break;
        if (frame != FRAMES || !summary(names[phase], first, frame, missed, period)) {
            puts("GCM_PACING_FAIL phase accounting"); failed = 1; break;
        }
        ++completed;
    }
    cellGcmSetFlipHandler(NULL);
    if (cellSysutilUnregisterCallback(0)) { puts("GCM_PACING_FAIL callback cleanup"); failed = 1; }
    if (failed) { printf("GCM_PACING_FAIL completed=%u\n", completed); return 1; }
    if (atomic_load_explicit(&quit_requested, memory_order_acquire)) {
        printf("GCM_PACING_QUIT completed=%u\n", completed); return 0;
    }
    printf("GCM_PACING_OK phases=%u frames=%u\n", completed, completed * FRAMES);
    return completed == PHASES ? 0 : 1;
}
