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
    last_event_ = buf;
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
    small_level_ = lv.small;
    large_level_ = lv.large;
    last_command_ = Command::levels;
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
        stop_gave_up_ = false;
        last_command_ = Command::stop;
        return;
    }
    ++n_.stop_failures;
    say("DS3B_PULSE stop_failed why=%s code=0x%08x retries_left=%u", why,
        unsigned(pad_.first_error_code()), stop_retries_left_);
    pulse_active_ = false;
    if (stop_retries_left_ == 0) {
        stop_pending_ = false;
        stop_gave_up_ = true;  // stays on screen until a stop succeeds
        say("DS3B_PULSE stop_gave_up why=%s", why);
        return;
    }
    --stop_retries_left_;
    stop_pending_ = true;
}

// Cached input belongs to one connection of one controller. Called on every
// acquire, reconnect, port change and disconnect.
void Bench::forget_controller()
{
    have_record_ = false;
    last_command_ = Command::none;
    have_analog_ = false;
    printed_analog_ = false;
    last_buttons_ = 0;
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
        forget_controller();
        say("DS3B_PORT disconnected port=%d", pad_.port());
    } else if (!was_connected_ && connected) {
        forget_controller();  // a new selection or a reconnect: no old values
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
            forget_controller();
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
    last_record_ = d;
    have_record_ = true;
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
            // A fresh acquire, even of the same port: drop cached values now,
            // before this iteration draws, and log the port on the next step.
            forget_controller();
            was_connected_ = false;
            if (pad_.port() != from)
                ++n_.port_changes;
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

std::string Screen::text() const
{
    std::string out;
    for (const Row &r : rows) {
        for (const Segment &s : r.segments)
            out += s.text;
        out += '\n';
    }
    return out;
}

namespace {

struct ButtonName {
    int offset;  // CELL_PAD_BTN_OFFSET_DIGITAL1 or _DIGITAL2
    std::uint16_t bit;
    const char *name;
};

// Plain ASCII: the debug font has no symbol glyphs.
constexpr ButtonName kRow1[] = {
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_UP, "UP"},
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_DOWN, "DOWN"},
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_LEFT, "LEFT"},
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_RIGHT, "RIGHT"},
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_SELECT, "SELECT"},
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_START, "START"},
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_L3, "L3"},
    {CELL_PAD_BTN_OFFSET_DIGITAL1, CELL_PAD_CTRL_R3, "R3"},
};
constexpr ButtonName kRow2[] = {
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CROSS, "CROSS"},
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_CIRCLE, "CIRCLE"},
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_SQUARE, "SQUARE"},
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_TRIANGLE, "TRIANGLE"},
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L1, "L1"},
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R1, "R1"},
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_L2, "L2"},
    {CELL_PAD_BTN_OFFSET_DIGITAL2, CELL_PAD_CTRL_R2, "R2"},
};

struct PressName {
    int offset;
    const char *name;
};
constexpr PressName kPress[] = {
    {CELL_PAD_BTN_OFFSET_PRESS_UP, "UP"},       {CELL_PAD_BTN_OFFSET_PRESS_DOWN, "DN"},
    {CELL_PAD_BTN_OFFSET_PRESS_LEFT, "LT"},     {CELL_PAD_BTN_OFFSET_PRESS_RIGHT, "RT"},
    {CELL_PAD_BTN_OFFSET_PRESS_CROSS, "X"},     {CELL_PAD_BTN_OFFSET_PRESS_CIRCLE, "O"},
    {CELL_PAD_BTN_OFFSET_PRESS_SQUARE, "SQ"},   {CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE, "TR"},
    {CELL_PAD_BTN_OFFSET_PRESS_L1, "L1"},       {CELL_PAD_BTN_OFFSET_PRESS_R1, "R1"},
    {CELL_PAD_BTN_OFFSET_PRESS_L2, "L2"},       {CELL_PAD_BTN_OFFSET_PRESS_R2, "R2"},
};

std::string fmt(const char *f, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

}  // namespace

Screen Bench::screen() const
{
    Screen s;
    auto row = [&s]() -> Row & {
        s.rows.emplace_back();
        return s.rows.back();
    };
    auto add = [](Row &r, std::string text, Ink ink) { r.segments.push_back({std::move(text), ink}); };

    {
        Row &r = row();
        add(r, "PS3DK DS3 BENCH", Ink::title);
        if (pad_.port_connected()) {
            const PortCaps &c = pad_.caps();
            add(r,
                fmt("   port %d   pressure:%s  sixaxis:%s  rumble:%s", pad_.port(),
                    !c.press ? "none" : pad_.press_active() ? "ON" : "off",
                    !c.sensor ? "none" : pad_.sensor_active() ? "ON" : "off",
                    c.actuator ? "yes" : "none"),
                Ink::normal);
        }
    }
    add(row(), "L1 next pad   R1 stop   CROSS small   SQUARE large 64", Ink::dim);
    add(row(), "TRIANGLE both (large 128)   CIRCLE large 255   pulses 200 ms", Ink::dim);
    add(row(), "SELECT+START exit", Ink::dim);

    if (!pad_.port_connected()) {
        add(row(), "No controller on the selected port: connect a DualShock 3.", Ink::warn);
    } else if (!have_record_) {
        add(row(), "Waiting for the first record from the controller.", Ink::dim);
    } else {
        const CellPadData &d = last_record_;
        auto buttons = [&](const ButtonName *list, std::size_t n, const char *label) {
            Row &r = row();
            add(r, label, Ink::normal);
            for (std::size_t i = 0; i < n; ++i) {
                const bool down = d.button[list[i].offset] & list[i].bit;
                add(r, fmt("%s ", list[i].name), down ? Ink::on : Ink::dim);
            }
        };
        buttons(kRow1, sizeof kRow1 / sizeof kRow1[0], "Buttons  ");
        buttons(kRow2, sizeof kRow2 / sizeof kRow2[0], "         ");
        const auto v = [&](int i) { return unsigned(d.button[i] & 0xff); };
        if (d.len >= kLenSticks) {
            add(row(),
                fmt("Left stick  X %3u  Y %3u      Right stick  X %3u  Y %3u",
                    v(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X), v(CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y),
                    v(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_X), v(CELL_PAD_BTN_OFFSET_ANALOG_RIGHT_Y)),
                Ink::normal);
        } else {
            add(row(), "Sticks: not in this record", Ink::dim);
        }
        // A mode can be on while a record still lacks its fields: say which.
        auto absent = [&](bool cap, bool wanted, bool active, const char *what) {
            if (!cap)
                return fmt("%s: not supported by this port", what);
            if (!wanted)
                return fmt("%s: not requested by this build", what);
            if (!active)
                return fmt("%s: not enabled (the enable request failed)", what);
            return fmt("%s: enabled, but not in this record (len %d)", what, int(d.len));
        };
        if (pad_.press_active() && d.len >= kLenPressure) {
            // Two rows of six, so every row fits the screen.
            for (std::size_t half = 0; half < 2; ++half) {
                Row &r = row();
                add(r, half == 0 ? "Pressure " : "         ", Ink::normal);
                for (std::size_t i = half * 6; i < half * 6 + 6; ++i) {
                    const PressName &p = kPress[i];
                    add(r, fmt("%-2s %3u   ", p.name, v(p.offset)), v(p.offset) ? Ink::on : Ink::dim);
                }
            }
        } else {
            add(row(), absent(pad_.caps().press, pad_.wanted().press, pad_.press_active(), "Pressure"), Ink::dim);
        }
        if (pad_.sensor_active() && d.len >= kLenSensor) {
            const auto w = [&](int i) { return unsigned(d.button[i] & 0x3ff); };
            add(row(),
                fmt("Sixaxis raw  accel X %4u  Y %4u  Z %4u   G %4u", w(CELL_PAD_BTN_OFFSET_SENSOR_X),
                    w(CELL_PAD_BTN_OFFSET_SENSOR_Y), w(CELL_PAD_BTN_OFFSET_SENSOR_Z),
                    w(CELL_PAD_BTN_OFFSET_SENSOR_G)),
                Ink::normal);
            add(row(), "  10-bit raw counts; G is one raw gyro channel. Axes, centre", Ink::dim);
            add(row(), "  and units are not verified on hardware.", Ink::dim);
        } else {
            add(row(), absent(pad_.caps().sensor, pad_.wanted().sensor, pad_.sensor_active(), "Sixaxis"), Ink::dim);
        }
    }

    {
        // What was last ASKED of the motors: a command accepted by the API
        // is not proof that a motor turned, and nothing is sent to an
        // absent controller.
        Row &r = row();
        add(r, "Rumble   ", Ink::normal);
        if (!pad_.caps().actuator) {
            add(r, "not supported by this port", Ink::dim);
        } else if (!pad_.port_connected()) {
            add(r, "unknown: controller disconnected, no stop could be sent", Ink::warn);
        } else if (last_command_ == Command::levels && pad_.motors_active()) {
            add(r, fmt("last command: small %s, large %u", small_level_ ? "ON" : "off", unsigned(large_level_)),
                Ink::on);
        } else if (last_command_ == Command::stop) {
            add(r, "last command: stop", Ink::dim);
        } else {
            add(r, "no command sent to this controller yet", Ink::dim);
        }
    }
    if (stop_pending_)
        add(row(), "STOP FAILED: retrying the zero write", Ink::warn);
    if (stop_gave_up_)
        add(row(), "STOP FAILED after every retry: the motors may still be running", Ink::warn);
    const std::uint64_t now = clock_.now_us(clock_.ctx);
    const std::uint64_t used = now - start_;
    const unsigned left = used >= cfg_.session_us ? 0u : unsigned((cfg_.session_us - used) / 1000000);
    add(row(),
        fmt("Records  fresh %u  no-new %u  malformed %u", n_.fresh, n_.no_new_data, n_.malformed), Ink::dim);
    add(row(), fmt("Disconnects %u   time left %u s", n_.disconnects, left), Ink::dim);
    if (!last_event_.empty()) {
        std::string e = last_event_;
        if (e.size() > kScreenColumns - 6)
            e.resize(kScreenColumns - 6);  // after the "Last: " prefix
        const bool bad = e.find("refused") != std::string::npos || e.find("failed") != std::string::npos;
        add(row(), "Last: " + e, bad ? Ink::warn : Ink::dim);
    }
    return s;
}

Iteration iterate(PadLifecycle &pad, Bench &bench, const DisplayOps *display, DisplayStats &stats)
{
    if (pad.refresh() != CELL_OK)
        return Iteration::refresh_failed;
    if (!bench.step())
        return Iteration::ended;
    if (display && !stats.stopped) {
        const int r = display->ready(display->ctx);
        if (r < 0) {
            stats.stopped = true;
        } else if (r == 0) {
            ++stats.skipped;
        } else if (display->draw(display->ctx, bench.screen()) < 0) {
            stats.stopped = true;
        } else {
            ++stats.drawn;
        }
    }
    return Iteration::running;
}

void Bench::finish()
{
    if (pulse_active_ || stop_pending_ || pad_.motors_active())
        stop_pulse(clock_.now_us(clock_.ctx), "finish");
}

}  // namespace ds3b
