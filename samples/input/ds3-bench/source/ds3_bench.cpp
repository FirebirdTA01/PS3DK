#include "ds3_bench.h"

#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace ds3b {

namespace {

constexpr std::uint16_t kSelect = CELL_PAD_CTRL_SELECT << 8;
constexpr std::uint16_t kStart = CELL_PAD_CTRL_START << 8;
constexpr std::uint16_t kExitCombo = kSelect | kStart;
constexpr std::uint16_t kL1 = CELL_PAD_CTRL_L1;
constexpr std::uint16_t kR1 = CELL_PAD_CTRL_R1;
constexpr std::uint16_t kCross = CELL_PAD_CTRL_CROSS;
constexpr std::uint16_t kSquare = CELL_PAD_CTRL_SQUARE;
constexpr std::uint16_t kTriangle = CELL_PAD_CTRL_TRIANGLE;
constexpr std::uint16_t kCircle = CELL_PAD_CTRL_CIRCLE;

constexpr int kFirstAnalog = CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X;
constexpr int kFirstPress = CELL_PAD_BTN_OFFSET_PRESS_RIGHT;

struct Levels {
    std::uint8_t small, large;
};

Levels levels_of(Action a)
{
    switch (a) {
    case Action::small_only: return {1, 0};
    case Action::large_low: return {0, kLargeLow};
    case Action::both: return {1, kLargeBoth};
    case Action::large_high: return {0, kLargeHigh};
    default: return {0, 0};
    }
}

}  // namespace

const char *action_name(Action a)
{
    switch (a) {
    case Action::none: return "none";
    case Action::exit: return "exit";
    case Action::stop: return "stop";
    case Action::next_port: return "next_port";
    case Action::small_only: return "small_only";
    case Action::large_low: return "large_low";
    case Action::both: return "both";
    case Action::large_high: return "large_high";
    }
    return "unknown";
}

const char *end_name(End e)
{
    switch (e) {
    case End::running: return "running";
    case End::exit_combo: return "exit_combo";
    case End::exit_request: return "exit_request";
    case End::session_deadline: return "session_deadline";
    case End::read_failed: return "read_failed";
    }
    return "unknown";
}

std::uint16_t buttons_of(const CellPadData &d)
{
    return static_cast<std::uint16_t>(((d.button[CELL_PAD_BTN_OFFSET_DIGITAL1] & 0xff) << 8) |
                                      (d.button[CELL_PAD_BTN_OFFSET_DIGITAL2] & 0xff));
}

Action Edges::update(std::uint16_t pressed)
{
    ignored_ &= pressed;  // a released button is no longer ignored
    const std::uint16_t down = pressed & ~prev_ & ~ignored_;
    prev_ = pressed;
    if ((pressed & kExitCombo) == kExitCombo && (down & kExitCombo) && !(ignored_ & kExitCombo))
        return Action::exit;
    if (down & kR1) return Action::stop;
    if (down & kL1) return Action::next_port;
    if (down & kCross) return Action::small_only;
    if (down & kSquare) return Action::large_low;
    if (down & kTriangle) return Action::both;
    if (down & kCircle) return Action::large_high;
    return Action::none;
}

void Bench::say(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_.line(log_.ctx, buf);
}

void Bench::begin()
{
    start_ = clock_.now_us(clock_.ctx);
    say("DS3B_SESSION start pulse_ms=%u session_s=%u report_hz=%u deadband=%d",
        unsigned(cfg_.pulse_us / 1000), unsigned(cfg_.session_us / 1000000),
        unsigned(cfg_.report_interval_us ? 1000000 / cfg_.report_interval_us : 0), cfg_.deadband);
}

void Bench::start_pulse(Action a, std::uint64_t now)
{
    const Levels lv = levels_of(a);
    if (!pad_.caps().actuator) {
        ++n_.pulses_refused;
        say("DS3B_PULSE refused action=%s reason=no_actuator_capability", action_name(a));
        return;
    }
    if (!pad_.set_motors(lv.small, lv.large)) {
        ++n_.pulses_refused;
        say("DS3B_PULSE refused action=%s reason=write_failed code=0x%08x", action_name(a),
            unsigned(pad_.first_error_code()));
        return;
    }
    if (pulse_active_)
        say("DS3B_PULSE replaced elapsed_us=%" PRIu64, now - pulse_start_);
    pulse_active_ = true;
    stop_pending_ = false;
    pulse_start_ = now;
    pulse_deadline_ = now + cfg_.pulse_us;
    ++n_.pulses_started;
    say("DS3B_PULSE start action=%s small=%u large=%u deadline_us=%" PRIu64, action_name(a),
        unsigned(lv.small), unsigned(lv.large), cfg_.pulse_us);
}

void Bench::stop_pulse(std::uint64_t now, const char *why)
{
    if (!stop_pending_)
        stop_retries_left_ = cfg_.stop_retry_steps;
    if (pad_.stop_motors()) {
        if (pulse_active_ || stop_pending_) {
            ++n_.pulses_stopped;
            say("DS3B_PULSE stop why=%s elapsed_us=%" PRIu64, why, now - pulse_start_);
        } else {
            say("DS3B_PULSE stop why=%s idle=1", why);
        }
        pulse_active_ = stop_pending_ = false;
        return;
    }
    ++n_.stop_failures;
    say("DS3B_PULSE stop_failed why=%s code=0x%08x retries_left=%u", why,
        unsigned(pad_.first_error_code()), stop_retries_left_);
    pulse_active_ = false;
    if (stop_retries_left_ == 0) {
        stop_pending_ = false;
        say("DS3B_PULSE stop_gave_up why=%s", why);
        return;
    }
    --stop_retries_left_;
    stop_pending_ = true;
}

bool Bench::analog_changed(const CellPadData &d) const
{
    if (!have_analog_)
        return true;
    const int last = (pad_.press_active() && d.len >= kLenPressure) ? kLenPressure : kLenSticks;
    for (int i = kFirstAnalog; i < last; ++i) {
        const int delta = int(d.button[i] & 0xff) - int(last_analog_[i]);
        if (std::abs(delta) > cfg_.deadband)
            return true;
    }
    return false;
}

void Bench::remember_analog(const CellPadData &d)
{
    const int last = d.len >= kLenPressure ? kLenPressure : kLenSticks;
    for (int i = kFirstAnalog; i < last; ++i)
        last_analog_[i] = static_cast<std::uint8_t>(d.button[i] & 0xff);
    have_analog_ = true;
}

void Bench::report(const CellPadData &d, std::uint64_t now)
{
    const std::uint16_t b = buttons_of(d);
    if (b != last_buttons_) {
        ++n_.digital_lines;
        say("DS3B_BUTTONS d1=0x%02x d2=0x%02x", unsigned(b >> 8), unsigned(b & 0xff));
        last_buttons_ = b;
    }
    if (d.len < kLenSticks)
        return;
    if (!analog_changed(d))
        return;
    if (printed_analog_ && now - last_analog_print_ < cfg_.report_interval_us)
        return;
    ++n_.analog_lines;
    const auto v = [&](int i) { return unsigned(d.button[i] & 0xff); };
    if (pad_.press_active() && d.len >= kLenPressure) {
        say("DS3B_ANALOG L=(%u,%u) R=(%u,%u) press R=%u L=%u U=%u D=%u T=%u O=%u X=%u S=%u "
            "L1=%u R1=%u L2=%u R2=%u",
            v(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X), v(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y),
            v(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X), v(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y),
            v(kFirstPress + 0), v(kFirstPress + 1), v(kFirstPress + 2), v(kFirstPress + 3),
            v(kFirstPress + 4), v(kFirstPress + 5), v(kFirstPress + 6), v(kFirstPress + 7),
            v(kFirstPress + 8), v(kFirstPress + 9), v(kFirstPress + 10), v(kFirstPress + 11));
    } else {
        say("DS3B_ANALOG L=(%u,%u) R=(%u,%u) press=off",
            v(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X), v(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y),
            v(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X), v(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y));
    }
    remember_analog(d);
    last_analog_print_ = now;
    printed_analog_ = true;
}

bool Bench::step()
{
    if (end_ != End::running)
        return false;
    const std::uint64_t now = clock_.now_us(clock_.ctx);
    if (exit_requested_) {
        end_ = End::exit_request;
        return false;
    }
    if (now - start_ >= cfg_.session_us) {
        end_ = End::session_deadline;
        return false;
    }

    // The deadline is checked on every step, whatever the read returns.
    if (pulse_active_ && now >= pulse_deadline_)
        stop_pulse(now, "deadline");
    else if (stop_pending_)
        stop_pulse(now, "retry");

    const bool connected = pad_.port_connected();
    if (was_connected_ && !connected) {
        ++n_.disconnects;
        pulse_active_ = stop_pending_ = false;  // nothing can be written now
        say("DS3B_PORT disconnected port=%d", pad_.port());
    } else if (!was_connected_ && connected) {
        if (n_.disconnects)
            ++n_.reconnects;
        const PortCaps &c = pad_.caps();
        say("DS3B_PORT selected port=%d caps=0x%08x press=%u actuator=%u press_on=%u", pad_.port(),
            unsigned(c.raw), unsigned(c.press), unsigned(c.actuator), unsigned(pad_.press_active()));
    }
    was_connected_ = connected;

    if (!pad_.selected()) {
        pad_.select_next();
        return true;
    }

    CellPadData d{};
    switch (pad_.read(d)) {
    case Read::fresh: break;
    case Read::no_new_data: ++n_.no_new_data; return true;
    case Read::disconnected:
        if (was_connected_) {
            // NO_DEVICE from the read itself: same transition as Info2.
            ++n_.disconnects;
            pulse_active_ = stop_pending_ = false;
            was_connected_ = false;
            say("DS3B_PORT disconnected port=%d source=read", pad_.port());
        }
        return true;
    case Read::no_port: return true;
    case Read::malformed:
        ++n_.malformed;
        say("DS3B_READ malformed len=%d", int(pad_.rejected_len()));
        return true;
    case Read::failed:
        say("DS3B_READ failed code=0x%08x", unsigned(pad_.first_error_code()));
        end_ = End::read_failed;
        return false;
    }
    ++n_.fresh;
    if (pad_.suppress_held()) {
        edges_.suppress(buttons_of(d));
        pad_.clear_suppress_held();
    }
    const Action a = edges_.update(buttons_of(d));
    report(d, now);
    switch (a) {
    case Action::none: break;
    case Action::exit:
        say("DS3B_ACTION exit");
        end_ = End::exit_combo;
        return false;
    case Action::stop:
        say("DS3B_ACTION stop");
        stop_pulse(now, "R1");
        break;
    case Action::next_port: {
        const int from = pad_.port();
        if (pad_.select_next()) {
            pulse_active_ = stop_pending_ = false;  // release() wrote zero
            if (pad_.port() != from) {
                ++n_.port_changes;
                was_connected_ = false;  // log the new port on the next step
            }
            say("DS3B_ACTION next_port from=%d to=%d", from, pad_.port());
        } else if (pad_.port() == from && pad_.port_connected()) {
            // The old port's zero or restore failed: keep it, keep retrying.
            if (pad_.motors_active() && !stop_pending_) {
                stop_pending_ = true;
                stop_retries_left_ = cfg_.stop_retry_steps;
            }
            pulse_active_ = false;
            say("DS3B_ACTION next_port refused reason=cleanup_failed port=%d", from);
        }
        break;
    }
    default:
        say("DS3B_ACTION %s", action_name(a));
        start_pulse(a, now);
        break;
    }
    return true;
}

void Bench::finish()
{
    if (pulse_active_ || stop_pending_ || pad_.motors_active())
        stop_pulse(clock_.now_us(clock_.ctx), "finish");
}

}  // namespace ds3b
