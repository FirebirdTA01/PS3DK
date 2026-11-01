// DualShock 3 bench: buttons, sticks, pressure and two-motor rumble pulses.
// Binds the portable session (ds3_bench, pad_lifecycle) to cellPad, sysutil
// and the system clock. Output: an on-screen page (display.c) and TTY lines.
#include "display.h"
#include "ds3_bench.h"
#include "pad_lifecycle.h"

#include <atomic>
#include <cstdio>
#include <unistd.h>

#include <cell/pad.h>
#include <cell/sysutil.h>
#include <sys/process.h>
#include <sys/sys_time.h>

SYS_PROCESS_PARAM(1001, 0x100000);

using namespace ds3b;

static_assert(sizeof(CellPadData) == 132, "CellPadData layout");
static_assert(sizeof(CellPadActParam) == 8, "CellPadActParam layout");
static_assert(std::atomic<unsigned>::is_always_lock_free, "callback flag must be lock free");

static std::atomic<unsigned> quitting{0};

static void on_sysutil(std::uint64_t status, std::uint64_t, void *)
{
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME)
        quitting.store(1, std::memory_order_release);
}

static std::int32_t pad_init(void *, std::uint32_t max) { return cellPadInit(max); }
static std::int32_t pad_end(void *) { return cellPadEnd(); }
static std::int32_t pad_info(void *, CellPadInfo2 *info) { return cellPadGetInfo2(info); }
static std::int32_t pad_data(void *, std::uint32_t port, CellPadData *d) { return cellPadGetData(port, d); }
static std::int32_t pad_setting(void *, std::uint32_t port, std::uint32_t s) { return cellPadSetPortSetting(port, s); }
static std::int32_t pad_act(void *, std::uint32_t port, CellPadActParam *p) { return cellPadSetActDirect(port, p); }
static std::uint64_t now_us(void *) { return static_cast<std::uint64_t>(sys_time_get_system_time()); }
static void tty(void *, const char *text) { std::printf("%s\n", text); }

static std::uint32_t colour(Ink ink)
{
    switch (ink) {
    case Ink::title: return 0xffffffff;
    case Ink::normal: return 0xffe0e0e0;
    case Ink::dim: return 0xff8890a0;
    case Ink::on: return 0xff60ff60;
    case Ink::warn: return 0xffff9040;
    }
    return 0xffffffff;
}

// One page. The debug font advances about 0.0147 screen widths per
// character at scale 1.0 (0.022 at 1.5, as hello-ppu-dbgfont measures), so
// kScreenColumns characters at 0.8 end at about 0.97 of the width. Pending
// visual check on hardware and RPCS3.
static int display_ready(void *) { return ds3_display_ready(); }
static int display_draw(void *, const Screen &s)
{
    constexpr float scale = 0.8f, advance = 0.0147f * scale, line = 0.045f;
    ds3_display_begin();
    float y = 0.05f;
    for (const Row &r : s.rows) {
        float x = 0.05f;
        for (const Segment &g : r.segments) {
            ds3_display_text(x, y, scale, colour(g.ink), g.text.c_str());
            x += advance * float(g.text.size());
        }
        y += line;
    }
    return ds3_display_end();
}

int main()
{
    std::printf("DS3B_START controls: L1 next port, R1 stop, CROSS small, SQUARE large64, "
                "TRIANGLE both(large128), CIRCLE large255, SELECT+START exit\n");
    int rc = cellSysutilRegisterCallback(0, on_sysutil, nullptr);
    if (rc != 0) {
        std::printf("DS3B_FAIL cellSysutilRegisterCallback=0x%08x\n", unsigned(rc));
        return 1;
    }
    const PadOps ops{pad_init, pad_end, pad_info, pad_data, pad_setting, pad_act, nullptr};
    Wanted wanted;
    wanted.press = true;
    wanted.sensor = true;  // Sixaxis, where the port reports it
    PadLifecycle pad(ops, wanted);
    const int display_rc = ds3_display_open();
    if (display_rc != 0)
        std::printf("DS3B_DISPLAY unavailable rc=%d: TTY only\n", display_rc);
    rc = pad.open();
    if (rc != CELL_OK) {
        std::printf("DS3B_FAIL cellPadInit=0x%08x\n", unsigned(rc));
        cellSysutilUnregisterCallback(0);
        return 1;
    }
    Bench bench(pad, ClockOps{now_us, nullptr}, LogOps{tty, nullptr});
    bench.begin();
    const DisplayOps display{display_ready, display_draw, nullptr};
    DisplayStats stats;
    bool display_reported = false;
    int callback_rc = 0;
    for (;;) {
        callback_rc = cellSysutilCheckCallback();
        if (callback_rc != 0)
            break;
        if (quitting.load(std::memory_order_acquire))
            bench.request_exit();
        // Never waits on the display: a pending flip skips the frame.
        const Iteration it = iterate(pad, bench, display_rc == 0 ? &display : nullptr, stats);
        if (it == Iteration::refresh_failed) {
            std::printf("DS3B_FAIL cellPadGetInfo2=0x%08x\n", unsigned(pad.first_error_code()));
            break;
        }
        if (it == Iteration::ended)
            break;
        if (stats.stopped && !display_reported) {
            std::printf("DS3B_DISPLAY stopped after a submission error: TTY only from here\n");
            display_reported = true;
        }
        usleep(10000);  // the loop period, independent of the display rate
    }
    std::printf("DS3B_DISPLAY frames drawn=%u skipped_pending_flip=%u stopped=%u\n", stats.drawn,
                stats.skipped, unsigned(stats.stopped));
    bench.finish();
    pad.close();
    ds3_display_close();
    const int unreg = cellSysutilUnregisterCallback(0);
    const Counters &n = bench.counters();
    std::printf("DS3B_SUMMARY end=%s fresh=%u no_new=%u malformed=%u disconnects=%u reconnects=%u "
                "pulses=%u refused=%u stopped=%u stop_failures=%u lifecycle_stop_failures=%u "
                "restore_failures=%u port_changes=%u\n",
                end_name(bench.end()), n.fresh, n.no_new_data, n.malformed, n.disconnects,
                n.reconnects, n.pulses_started, n.pulses_refused, n.pulses_stopped, n.stop_failures,
                pad.stop_failures(), pad.restore_failures(), n.port_changes);
    if (pad.first_error())
        std::printf("DS3B_FIRST_ERROR %s=0x%08x\n", pad.first_error(), unsigned(pad.first_error_code()));
    const bool clean = callback_rc == 0 && unreg == 0 && bench.end() != End::read_failed &&
                       bench.end() != End::running && pad.stop_failures() == 0 &&
                       pad.restore_failures() == 0;
    // API success only: whether a motor physically ran is an operator observation.
    std::printf("%s callback_rc=0x%08x unregister_rc=0x%08x\n", clean ? "DS3B_API_OK" : "DS3B_FAIL",
                unsigned(callback_rc), unsigned(unreg));
    return clean ? 0 : 1;
}
