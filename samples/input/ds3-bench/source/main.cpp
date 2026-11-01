// DualShock 3 bench: buttons, sticks, pressure and two-motor rumble pulses.
// Binds the portable session (ds3_bench, pad_lifecycle) to cellPad, sysutil
// and the system clock. Output is TTY only.
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
    PadLifecycle pad(ops, wanted);
    rc = pad.open();
    if (rc != CELL_OK) {
        std::printf("DS3B_FAIL cellPadInit=0x%08x\n", unsigned(rc));
        cellSysutilUnregisterCallback(0);
        return 1;
    }
    Bench bench(pad, ClockOps{now_us, nullptr}, LogOps{tty, nullptr});
    bench.begin();
    int callback_rc = 0;
    for (;;) {
        callback_rc = cellSysutilCheckCallback();
        if (callback_rc != 0)
            break;
        if (quitting.load(std::memory_order_acquire))
            bench.request_exit();
        if (pad.refresh() != CELL_OK) {
            std::printf("DS3B_FAIL cellPadGetInfo2=0x%08x\n", unsigned(pad.first_error_code()));
            break;
        }
        if (!bench.step())
            break;
        usleep(10000);
    }
    bench.finish();
    pad.close();
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
