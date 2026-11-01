// DualShock 3 bench session: control mapping with fresh-edge detection,
// non-blocking bounded rumble pulses, and a rate-limited change reporter.
// Portable; time and output come through injected callbacks.
#pragma once

#include "pad_lifecycle.h"
#include <cstdint>

namespace ds3b {

struct ClockOps {
    std::uint64_t (*now_us)(void *ctx);  // monotonic microseconds
    void *ctx;
};

struct LogOps {
    void (*line)(void *ctx, const char *text);  // one complete line, no newline
    void *ctx;
};

struct Config {
    std::uint64_t pulse_us = 200000;          // every pulse: 200 ms
    std::uint64_t session_us = 120000000;     // default session: 120 s
    std::uint64_t report_interval_us = 100000;  // analog/pressure lines: at most 10 Hz
    int deadband = 2;                         // raw units vs last displayed value
    unsigned stop_retry_steps = 10;           // steps a failed stop is retried for
};

// Large-motor levels; the small motor is on/off only.
constexpr std::uint8_t kLargeLow = 64;
constexpr std::uint8_t kLargeBoth = 128;  // only with the small motor (TRIANGLE)
constexpr std::uint8_t kLargeHigh = 255;

// Controls: one action per fresh press. Priority: exit, stop, next port,
// then pulses in the order CROSS, SQUARE, TRIANGLE, CIRCLE.
enum class Action {
    none,
    exit,         // SELECT+START
    stop,         // R1
    next_port,    // L1
    small_only,   // CROSS: motor[0]=1, motor[1]=0
    large_low,    // SQUARE: motor[0]=0, motor[1]=64
    both,         // TRIANGLE: motor[0]=1, motor[1]=128
    large_high,   // CIRCLE: motor[0]=0, motor[1]=255
};

const char *action_name(Action a);

// Pressed buttons as (DIGITAL1 << 8) | DIGITAL2.
std::uint16_t buttons_of(const CellPadData &d);

// Fresh-edge decoder with held-button suppression.
class Edges {
public:
    // Ignore every button currently held until it is released.
    void suppress(std::uint16_t held) { ignored_ = held; }
    // Feed the current pressed set; returns the highest-priority action
    // from buttons that went down this step and are not ignored.
    Action update(std::uint16_t pressed);
    std::uint16_t ignored() const { return ignored_; }

private:
    std::uint16_t prev_ = 0;
    std::uint16_t ignored_ = 0;
};

enum class End {
    running,
    exit_combo,
    exit_request,      // sysutil EXITGAME
    session_deadline,
    read_failed,
};

const char *end_name(End e);

struct Counters {
    unsigned fresh = 0, no_new_data = 0, malformed = 0, disconnects = 0, reconnects = 0;
    unsigned pulses_started = 0, pulses_refused = 0, pulses_stopped = 0, stop_failures = 0;
    unsigned port_changes = 0, digital_lines = 0, analog_lines = 0;
};

class Bench {
public:
    Bench(PadLifecycle &pad, ClockOps clock, LogOps log, Config cfg = Config())
        : pad_(pad), clock_(clock), log_(log), cfg_(cfg) {}

    void begin();
    // One loop iteration, after PadLifecycle::refresh(). False once the
    // session has ended; end() says why.
    bool step();
    void request_exit() { exit_requested_ = true; }
    // Stop any pulse before cleanup. Safe to call more than once.
    void finish();

    End end() const { return end_; }
    const Counters &counters() const { return n_; }
    bool pulse_active() const { return pulse_active_; }
    std::uint64_t pulse_deadline() const { return pulse_deadline_; }

private:
    void say(const char *fmt, ...);
    void start_pulse(Action a, std::uint64_t now);
    void stop_pulse(std::uint64_t now, const char *why);
    void report(const CellPadData &d, std::uint64_t now);
    bool analog_changed(const CellPadData &d) const;
    void remember_analog(const CellPadData &d);

    PadLifecycle &pad_;
    ClockOps clock_;
    LogOps log_;
    Config cfg_;
    Edges edges_;
    Counters n_;
    End end_ = End::running;
    bool exit_requested_ = false;
    std::uint64_t start_ = 0;
    bool pulse_active_ = false;
    std::uint64_t pulse_start_ = 0, pulse_deadline_ = 0;
    unsigned stop_retries_left_ = 0;
    bool stop_pending_ = false;
    bool was_connected_ = false;
    std::uint16_t last_buttons_ = 0;
    bool have_analog_ = false;
    std::uint8_t last_analog_[CELL_PAD_BTN_OFFSET_PRESS_R2 + 1] = {};
    std::uint64_t last_analog_print_ = 0;
    bool printed_analog_ = false;
};

}  // namespace ds3b
