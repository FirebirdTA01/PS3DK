// Host test support: a scripted fake cellPad, a fake clock and a log capture.
#pragma once

#include "../source/ds3_bench.h"
#include "../source/pad_lifecycle.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace fake {

struct Act {
    std::uint32_t port;
    std::uint8_t small, large;
};
struct Setting {
    std::uint32_t port, value;
};

struct Pad {
    std::int32_t init_rc = CELL_OK, end_rc = CELL_OK, info_rc = CELL_OK;
    CellPadInfo2 info{};
    std::deque<std::int32_t> data_rc;  // consumed per get_data; empty means CELL_OK
    std::deque<CellPadData> records;   // consumed per OK get_data; empty means len 0
    std::deque<std::int32_t> setting_rc, act_rc;
    std::vector<Setting> settings;     // every set_port_setting call
    std::vector<Act> acts;             // every set_act_direct call
    unsigned inits = 0, ends = 0, reads = 0;
    std::vector<std::uint32_t> read_ports;

    void connect(std::uint32_t port, std::uint32_t caps, std::uint32_t setting = 0)
    {
        info.port_status[port] = CELL_PAD_STATUS_CONNECTED;
        info.device_capability[port] = caps;
        info.port_setting[port] = setting;
    }
    void disconnect(std::uint32_t port) { info.port_status[port] = CELL_PAD_STATUS_DISCONNECTED; }
};

inline std::int32_t init(void *c, std::uint32_t) { auto &p = *static_cast<Pad *>(c); ++p.inits; return p.init_rc; }
inline std::int32_t end(void *c) { auto &p = *static_cast<Pad *>(c); ++p.ends; return p.end_rc; }
inline std::int32_t info2(void *c, CellPadInfo2 *i)
{
    auto &p = *static_cast<Pad *>(c);
    if (p.info_rc == CELL_OK)
        *i = p.info;
    return p.info_rc;
}
inline std::int32_t data(void *c, std::uint32_t port, CellPadData *d)
{
    auto &p = *static_cast<Pad *>(c);
    ++p.reads;
    p.read_ports.push_back(port);
    std::int32_t rc = CELL_OK;
    if (!p.data_rc.empty()) {
        rc = p.data_rc.front();
        p.data_rc.pop_front();
    }
    if (rc != CELL_OK)
        return rc;
    if (p.records.empty()) {
        d->len = 0;
    } else {
        *d = p.records.front();
        p.records.pop_front();
    }
    return CELL_OK;
}
inline std::int32_t setting(void *c, std::uint32_t port, std::uint32_t v)
{
    auto &p = *static_cast<Pad *>(c);
    p.settings.push_back({port, v});
    std::int32_t rc = CELL_OK;
    if (!p.setting_rc.empty()) {
        rc = p.setting_rc.front();
        p.setting_rc.pop_front();
    }
    return rc;
}
inline std::int32_t act(void *c, std::uint32_t port, CellPadActParam *a)
{
    auto &p = *static_cast<Pad *>(c);
    p.acts.push_back({port, a->motor[0], a->motor[1]});
    std::int32_t rc = CELL_OK;
    if (!p.act_rc.empty()) {
        rc = p.act_rc.front();
        p.act_rc.pop_front();
    }
    return rc;
}

inline ds3b::PadOps ops(Pad &p) { return {init, end, info2, data, setting, act, &p}; }

struct Clock {
    std::uint64_t now = 1000000;
};
inline std::uint64_t now_us(void *c) { return static_cast<Clock *>(c)->now; }
inline ds3b::ClockOps clock(Clock &c) { return {now_us, &c}; }

struct Log {
    std::vector<std::string> lines;
    bool has(const char *needle) const
    {
        for (const auto &l : lines)
            if (l.find(needle) != std::string::npos)
                return true;
        return false;
    }
    unsigned count(const char *needle) const
    {
        unsigned n = 0;
        for (const auto &l : lines)
            if (l.find(needle) != std::string::npos)
                ++n;
        return n;
    }
};
inline void line(void *c, const char *t) { static_cast<Log *>(c)->lines.emplace_back(t); }
inline ds3b::LogOps log(Log &l) { return {line, &l}; }

constexpr std::uint32_t kAll = CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE |
                               CELL_PAD_CAPABILITY_SENSOR_MODE | CELL_PAD_CAPABILITY_ACTUATOR;

// A full-length record with sticks centred and no pressure.
inline CellPadData record(std::uint8_t d1 = 0, std::uint8_t d2 = 0, std::int32_t len = 24)
{
    CellPadData d{};
    d.len = len;
    d.button[CELL_PAD_BTN_OFFSET_DIGITAL1] = d1;
    d.button[CELL_PAD_BTN_OFFSET_DIGITAL2] = d2;
    for (int i = CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X; i <= CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y; ++i)
        d.button[i] = 0x80;
    return d;
}

}  // namespace fake

// A whole session over the fake pad: pressure wanted, port 0 selected by the
// first step. step(rec) queues one record (or none) and runs one iteration.
struct Rig {
    fake::Pad pad;
    fake::Clock clk;
    fake::Log log;
    ds3b::PadLifecycle lc;
    ds3b::Bench bench;
    explicit Rig(std::uint32_t caps = fake::kAll, ds3b::Config cfg = ds3b::Config())
        : lc(fake::ops(pad), wanted()), bench(lc, fake::clock(clk), fake::log(log), cfg)
    {
        pad.connect(0, caps);
    }
    static ds3b::Wanted wanted()
    {
        ds3b::Wanted w;
        w.press = true;
        return w;
    }
    // settle: feed one record with nothing held, consuming the held-button
    // suppression armed by the acquire, so the test's first press counts.
    void start(bool settle = true)
    {
        lc.open();
        lc.refresh();
        bench.begin();
        step_none();  // selects port 0
        step_none();  // logs the selection
        if (settle)
            step(fake::record());
    }
    bool step(const CellPadData &rec)
    {
        pad.records.push_back(rec);
        lc.refresh();
        return bench.step();
    }
    bool step_none()
    {
        lc.refresh();
        return bench.step();
    }
    void advance_ms(std::uint64_t ms) { clk.now += ms * 1000; }
};

// Minimal checks: report every failure, exit 1 at the end if any failed.
static unsigned g_checks = 0, g_failed = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(x)) {                                                                 \
            ++g_failed;                                                             \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); \
        }                                                                           \
    } while (0)
static int finish_checks(const char *name)
{
    std::printf("%s checks=%u failed=%u\n", name, g_checks, g_failed);
    return g_failed ? 1 : 0;
}
