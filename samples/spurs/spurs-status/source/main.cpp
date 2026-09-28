/*
 * spurs-status -- run every spurs-suite row and show a live status board.
 *
 * A worker thread runs the rows in order; the main thread draws, every
 * frame:
 *   - the board: one line per row with its state (pending, running,
 *     OK, FAIL, INVALID, TIMEOUT), run time and verdict;
 *   - the live panel for the running row: what the PPU side is doing
 *     (starting SPURS, launching tasks, waiting, shutting down, joining),
 *     its recent activity, and each SPU task's result slot (still running
 *     or blocked, done, or failed with its rc and step).
 * The same verdict lines go to the TTY.  A row that fails leaves its SPURS
 * instance as it was, so the run stops there; a row still running after
 * kRowTimeout is marked TIMEOUT.  The board stays up until the system
 * asks the game to exit.
 */
#define SUITE_ROW "spurs-status"
#define SUITE_EMBEDDED
#include "harness.h"
#include "suite_rows.h"

#include <malloc.h>
#include <unistd.h>
#include <sys/sys_time.h>
#include <sysutil/video.h>
#include <cell/gcm.h>
#include <cell/dbgfont.h>
#include <cell/sysutil.h>
#include <rsx/rsx.h>

SYS_PROCESS_PARAM(1001, 0x100000);

suite::live_view &suite::live()
{
    static live_view v;
    return v;
}

namespace {

constexpr unsigned kRowCount = sizeof kRows / sizeof kRows[0];
constexpr uint64_t kRowTimeout = 60u * 1000u * 1000u;   /* us */

enum RowState { PENDING, RUNNING, PASSED, FAILED, INVALID, TIMEOUT, SKIPPED };

struct RowStatus {
    volatile int state;
    volatile uint64_t start, end;   /* us */
    char verdict[suite::TEXT * 2];
};

RowStatus g_rows[kRowCount];
volatile unsigned g_current = ~0u;
volatile bool g_done;
uint64_t g_t0;
volatile bool g_exit;

void on_sysutil(uint64_t status, uint64_t, void *)
{
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        g_exit = true;
}

uint64_t now_us() { return sys_time_get_system_time(); }

void worker(uint64_t)
{
    for (unsigned i = 0; i < kRowCount; ++i) {
        RowStatus &r = g_rows[i];
        suite::live_view &v = suite::live();
        v.slots = nullptr;
        v.nSlots = 0;
        v.logCount = 0;
        v.activity[0] = 0;
        v.verdict[0] = 0;
        r.start = now_us();
        g_current = i;
        r.state = RUNNING;
        int code = kRows[i].entry();
        r.end = now_us();
        std::memcpy(r.verdict, v.verdict, sizeof r.verdict);
        if (r.state == TIMEOUT)
            break;
        r.state = code == 0 ? PASSED : code == 1 ? FAILED : INVALID;
        if (code) {
            /* the failed row may still hold its SPUs */
            for (unsigned j = i + 1; j < kRowCount; ++j)
                g_rows[j].state = SKIPPED;
            break;
        }
    }
    unsigned passed = 0;
    for (const RowStatus &r : g_rows)
        passed += r.state == PASSED;
    std::printf("SPURS_STATUS DONE passed=%u of %u\n", passed, kRowCount);
    std::fflush(stdout);
    g_current = ~0u;
    g_done = true;
    sys_ppu_thread_exit(0);
}

/* ---- drawing ------------------------------------------------------------ */

constexpr uint32_t kWhite = 0xffe8eef4, kDim = 0xff6c7a89, kCyan = 0xff4fd6ff,
                   kGreen = 0xff4be37a, kRed = 0xffff5a5a, kAmber = 0xffffc03a,
                   kMagenta = 0xffe070ff, kBlue = 0xff7aa2ff;

const char kSpin[] = "|/-\\";

void text(float x, float y, float scale, uint32_t color, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cellDbgFontPuts(x, y, scale, color, buf);
}

void bar(char *out, unsigned width, unsigned filled)
{
    for (unsigned i = 0; i < width; ++i)
        out[i] = i < filled ? '#' : '.';
    out[width] = 0;
}

void draw(unsigned frame)
{
    const uint64_t t = now_us();
    const char spin = kSpin[(frame / 6) % 4];
    unsigned passed = 0, failed = 0, finished = 0;
    for (const RowStatus &r : g_rows) {
        passed += r.state == PASSED;
        failed += r.state == FAILED || r.state == INVALID || r.state == TIMEOUT;
        finished += r.state != PENDING && r.state != RUNNING;
    }

    /* header */
    text(0.04f, 0.04f, 1.25f, kCyan, "SPURS STATUS");
    text(0.30f, 0.05f, 0.9f, kDim, "PPU <-> SPU runtime check   %c   %4.1fs",
         g_done ? ' ' : spin, (double)(t - g_t0) / 1e6);
    char progress[41];
    bar(progress, 40, kRowCount ? finished * 40 / kRowCount : 0);
    text(0.04f, 0.09f, 0.9f, failed ? kRed : g_done ? kGreen : kBlue, "[%s] %u/%u", progress, finished, kRowCount);
    text(0.62f, 0.09f, 0.9f, kWhite, "pass %u   fail %u", passed, failed);

    /* board */
    float y = 0.15f;
    for (unsigned i = 0; i < kRowCount; ++i, y += 0.034f) {
        const RowStatus &r = g_rows[i];
        const char *badge = " --- ";
        uint32_t color = kDim;
        switch (r.state) {
        case RUNNING: badge = (frame / 20) % 2 ? " RUN " : " run "; color = kAmber; break;
        case PASSED:  badge = " OK  "; color = kGreen; break;
        case FAILED:  badge = " FAIL"; color = kRed; break;
        case INVALID: badge = " INV "; color = kRed; break;
        case TIMEOUT: badge = " TIME"; color = kMagenta; break;
        case SKIPPED: badge = " skip"; color = kDim; break;
        }
        uint64_t end = r.state == RUNNING || r.state == TIMEOUT ? t : r.end;
        text(0.04f, y, 0.8f, color, "[%s]", badge);
        text(0.13f, y, 0.8f, r.state == PENDING ? kDim : kWhite, "%-24s", kRows[i].name);
        if (r.state != PENDING && r.state != SKIPPED)
            text(0.40f, y, 0.8f, kDim, "%6.2fs", (double)(end - r.start) / 1e6);
        if (r.state == FAILED || r.state == INVALID)
            text(0.48f, y, 0.7f, kRed, "%.60s", r.verdict);
    }

    /* live panel for the running row */
    unsigned cur = g_current;
    const suite::live_view &v = suite::live();
    float py = 0.54f;
    text(0.04f, py, 0.9f, kCyan, "LIVE");
    if (cur < kRowCount) {
        RowStatus &r = g_rows[cur];
        if (r.state == RUNNING && t - r.start > kRowTimeout)
            r.state = TIMEOUT;
        text(0.12f, py, 0.9f, kWhite, "%s", kRows[cur].name);
        text(0.04f, py + 0.04f, 0.85f, kAmber, "%c PPU: %s", r.state == RUNNING ? spin : '!', v.activity);
        unsigned n = v.logCount < (unsigned)suite::LOG_LINES ? v.logCount : (unsigned)suite::LOG_LINES;
        for (unsigned k = 1; k < n; ++k) {
            unsigned idx = (v.logCount - 1 - k) % suite::LOG_LINES;
            text(0.06f, py + 0.04f + 0.03f * k, 0.7f, kDim, "%s", v.log[idx]);
        }
        /* SPU task slots */
        float sy = py + 0.25f;
        if (v.nSlots)
            text(0.04f, sy, 0.85f, kBlue, "SPU tasks");
        for (unsigned k = 0; k < v.nSlots; ++k) {
            const volatile result_slot &s = v.slots[k];
            float x = 0.04f + (k % 3) * 0.31f, yy = sy + 0.035f * (1 + k / 3);
            if (s.magic != (RESULT_MAGIC | k))
                text(x, yy, 0.7f, kDim, "task %2u  %c running / waiting", k, spin);
            else if (s.status == 0)
                text(x, yy, 0.7f, kGreen, "task %2u  done  v=%x", k, s.value);
            else
                text(x, yy, 0.7f, kRed, "task %2u  rc=%08x step %u", k, s.status, s.value);
        }
    } else if (g_done) {
        text(0.12f, py, 0.9f, failed ? kRed : kGreen,
             failed ? "run stopped at the first failing row" : "all rows passed");
    }
    text(0.04f, 0.95f, 0.65f, kDim, "4 SPUs per row  -  verdicts also on the TTY  -  timeout %us",
         (unsigned)(kRowTimeout / 1000000u));
}

/* ---- display setup (single-plane XRGB, double buffered) ------------------ */

constexpr uint32_t kCbSize = 0x100000, kHostSize = 32 * 1024 * 1024;

struct Buffer {
    uint32_t *ptr;
    uint32_t offset;
    uint16_t width, height;
};

uint32_t g_pitch, g_depthOffset;

bool make_buffer(Buffer &b, uint16_t w, uint16_t h, uint8_t id)
{
    b.ptr = static_cast<uint32_t *>(rsxMemalign(64, g_pitch * h));
    if (!b.ptr || cellGcmAddressToOffset(b.ptr, &b.offset) != 0)
        return false;
    b.width = w;
    b.height = h;
    return cellGcmSetDisplayBuffer(id, b.offset, g_pitch, w, h) == 0;
}

void set_target(CellGcmContextData *ctx, const Buffer &b)
{
    CellGcmSurface sf;
    std::memset(&sf, 0, sizeof sf);
    sf.colorFormat = GCM_SURFACE_X8R8G8B8;
    sf.colorTarget = GCM_SURFACE_TARGET_0;
    for (int i = 0; i < 4; ++i) {
        sf.colorLocation[i] = GCM_LOCATION_RSX;
        sf.colorPitch[i] = 64;
    }
    sf.colorOffset[0] = b.offset;
    sf.colorPitch[0] = g_pitch;
    sf.depthFormat = GCM_SURFACE_ZETA_Z16;
    sf.depthLocation = GCM_LOCATION_RSX;
    sf.depthOffset = g_depthOffset;
    sf.depthPitch = g_pitch;
    sf.type = GCM_SURFACE_TYPE_LINEAR;
    sf.antiAlias = GCM_SURFACE_CENTER_1;
    sf.width = b.width;
    sf.height = b.height;
    cellGcmSetSurface(ctx, &sf);
    float scale[4] = { b.width / 2.0f, -(float)b.height / 2.0f, 0.5f, 0.0f };
    float offset[4] = { b.width / 2.0f, b.height / 2.0f, 0.5f, 0.0f };
    cellGcmSetViewport(ctx, 0, 0, b.width, b.height, 0.0f, 1.0f, scale, offset);
    cellGcmSetScissor(ctx, 0, 0, b.width, b.height);
}

} // namespace

int main()
{
    std::printf("spurs-status: %u rows\n", kRowCount);
    g_t0 = now_us();

    void *host = memalign(1024 * 1024, kHostSize);
    videoState state;
    videoConfiguration vcfg;
    videoResolution res;
    if (!host || cellGcmInit(kCbSize, kHostSize, host) != 0)
        return 1;
    if (videoGetState(0, 0, &state) != 0 || state.state != 0
        || videoGetResolution(state.displayMode.resolution, &res) != 0)
        return 1;
    std::memset(&vcfg, 0, sizeof vcfg);
    vcfg.resolution = state.displayMode.resolution;
    vcfg.format = VIDEO_BUFFER_FORMAT_XRGB;
    vcfg.pitch = res.width * sizeof(uint32_t);
    vcfg.aspect = state.displayMode.aspect;
    if (videoConfigure(0, &vcfg, nullptr, 0) != 0)
        return 1;
    cellGcmSetFlipMode(GCM_FLIP_VSYNC);
    g_pitch = res.width * sizeof(uint32_t);
    void *depth = rsxMemalign(64, res.height * g_pitch);
    cellGcmAddressToOffset(depth, &g_depthOffset);
    cellGcmResetFlipStatus();

    Buffer bufs[2];
    for (uint8_t i = 0; i < 2; ++i)
        if (!make_buffer(bufs[i], res.width, res.height, i))
            return 1;

    size_t fontSize = CELL_DBGFONT_FRAGMENT_SIZE + CELL_DBGFONT_TEXTURE_SIZE + 12000 * CELL_DBGFONT_VERTEX_SIZE;
    CellDbgFontConfigGcm fcfg;
    std::memset(&fcfg, 0, sizeof fcfg);
    fcfg.localBufAddr = (sys_addr_t)(uintptr_t)rsxMemalign(128, fontSize);
    fcfg.localBufSize = (uint32_t)fontSize;
    fcfg.option = CELL_DBGFONT_VERTEX_LOCAL | CELL_DBGFONT_TEXTURE_LOCAL;
    if (cellDbgFontInitGcm(&fcfg) != 0)
        return 1;

    cellSysutilRegisterCallback(0, on_sysutil, nullptr);
    sys_ppu_thread_t t;
    if (sys_ppu_thread_create(&t, worker, 0, 1500, 0x10000, 0, "spurs-status rows") != 0)
        return 1;

    CellGcmContextData *ctx = CELL_GCM_CURRENT;
    unsigned cur = 0;
    for (unsigned frame = 0; !g_exit; ++frame) {
        cellSysutilCheckCallback();
        set_target(ctx, bufs[cur]);
        cellGcmSetClearColor(ctx, 0xff0b1119);
        cellGcmSetClearSurface(ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
        draw(frame);
        cellDbgFontDrawGcm();
        cellGcmFlush(ctx);
        while (cellGcmGetFlipStatus() != 0)
            usleep(200);
        cellGcmResetFlipStatus();
        if (cellGcmSetFlip(ctx, cur) == 0) {
            cellGcmFlush(ctx);
            cellGcmSetWaitFlip(ctx);
        }
        cur ^= 1;
    }
    cellDbgFontExitGcm();
    cellGcmFinish(ctx, 0);
    return 0;
}
