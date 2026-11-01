// Shared DualShock 3 port lifecycle: library init/end, port selection,
// capability checks, pressure-mode enable/restore, validated reads, actuator
// writes and reconnect. Portable: every cellPad call goes through PadOps, so
// host tests drive it with a fake pad. Reused by the DS3 motion sample.
#pragma once

#include <cell/pad.h>
#include <cstdint>

namespace ds3b {

// Sample-owned typed callbacks; main.cpp binds them to the real cellPad API.
struct PadOps {
    std::int32_t (*init)(void *ctx, std::uint32_t max_connect);
    std::int32_t (*end)(void *ctx);
    std::int32_t (*get_info2)(void *ctx, CellPadInfo2 *info);
    std::int32_t (*get_data)(void *ctx, std::uint32_t port, CellPadData *data);
    std::int32_t (*set_port_setting)(void *ctx, std::uint32_t port, std::uint32_t setting);
    std::int32_t (*set_act_direct)(void *ctx, std::uint32_t port, CellPadActParam *param);
    void *ctx;
};

// Number of button[] entries a record must hold before a field may be read.
constexpr std::int32_t kLenDigital = CELL_PAD_BTN_OFFSET_DIGITAL2 + 1;          // 4
constexpr std::int32_t kLenSticks = CELL_PAD_BTN_OFFSET_ANALOG_LEFT_Y + 1;      // 8
constexpr std::int32_t kLenPressure = CELL_PAD_BTN_OFFSET_PRESS_R2 + 1;         // 20
constexpr std::int32_t kLenSensor = CELL_PAD_BTN_OFFSET_SENSOR_G + 1;           // 24

enum class Read {
    fresh,         // CELL_OK, len >= kLenDigital: a new record
    no_new_data,   // CELL_OK, len == 0: success, nothing changed
    disconnected,  // CELL_PAD_ERROR_NO_DEVICE, or the port reports disconnected
    malformed,     // CELL_OK with len < 0, 0 < len < kLenDigital or len > CELL_PAD_MAX_CODES
    failed,        // any other error code
    no_port,       // nothing selected
};

const char *read_name(Read r);

// What the selected port can do, from the latest CellPadInfo2.
struct PortCaps {
    bool press = false;
    bool sensor = false;
    bool actuator = false;
    std::uint32_t raw = 0;
};

// Optional modes a sample asks for on acquire. Each is enabled only if the
// port reports the capability; the prior port_setting is restored on release.
struct Wanted {
    bool press = false;
    bool sensor = false;
};

class PadLifecycle {
public:
    PadLifecycle(const PadOps &ops, Wanted wanted) : ops_(ops), wanted_(wanted) {}

    // cellPadInit. Returns the API code; anything but CELL_OK leaves the
    // library closed and close() does nothing.
    std::int32_t open(std::uint32_t max_connect = CELL_PAD_MAX_PORT_NUM);

    // Refresh CellPadInfo2. Detects a disconnect or reconnect of the selected
    // port: a disconnect drops motor and setting state that can no longer be
    // written; a reconnect re-acquires (capabilities, settings) and arms the
    // held-button suppression. Returns the API code.
    std::int32_t refresh();

    // Bitmask of connected ports from the last refresh (bit n = port n).
    std::uint32_t connected_mask() const { return connected_mask_; }

    // Release the current port (motors to zero, setting restored) and take
    // the given one. Fails without selecting if the port is not connected or
    // the current port's cleanup failed.
    bool select(std::uint32_t port);
    // Select the next connected port after the current one (wrapping).
    bool select_next();
    // Release without selecting another port: zero the motors and restore
    // the prior setting. If either fails after its bounded retries, the port
    // stays selected (so later cleanup still addresses it) and false is
    // returned; select() then refuses to take another port.
    bool release();
    // release() then cellPadEnd(), even if the release failed (its failures
    // stay counted). Idempotent.
    void close();

    bool selected() const { return port_ >= 0; }
    int port() const { return port_; }
    bool port_connected() const { return selected() && port_connected_; }
    const PortCaps &caps() const { return caps_; }
    const Wanted &wanted() const { return wanted_; }
    bool press_active() const { return press_on_; }
    bool sensor_active() const { return sensor_on_; }

    // Read the selected port. On Read::fresh, out holds the record and out.len
    // is at least kLenDigital; fields beyond out.len must not be used. A
    // NO_DEVICE result is a disconnect, exactly as if CellPadInfo2 had
    // reported it: the next refresh() re-acquires the port.
    Read read(CellPadData &out);
    // The len of the last record rejected as malformed.
    std::int32_t rejected_len() const { return rejected_len_; }

    // Write both motors. small must be 0 or 1 (the DS3 small motor is
    // on/off only); large is 0..255. Refuses without a connected port or
    // without the actuator capability. Returns true on CELL_OK.
    bool set_motors(std::uint8_t small, std::uint8_t large);
    // True after a successful non-zero write and before a successful zero.
    bool motors_active() const { return motors_active_; }
    // Write zero, retrying a bounded number of times. Reports failure.
    bool stop_motors();

    // Set after every acquire: the sample must ignore buttons already held
    // until they are released (no stale edges after select or reconnect).
    bool suppress_held() const { return suppress_held_; }
    void clear_suppress_held() { suppress_held_ = false; }

    // First failure kept for the final report; later failures do not
    // overwrite it.
    const char *first_error() const { return first_error_; }
    std::int32_t first_error_code() const { return first_error_code_; }
    unsigned stop_failures() const { return stop_failures_; }
    unsigned restore_failures() const { return restore_failures_; }

    static constexpr unsigned kWriteAttempts = 3;

private:
    void note(const char *what, std::int32_t code);
    bool acquire(std::uint32_t port);
    bool restore_setting();
    void drop_connection();

    PadOps ops_;
    Wanted wanted_;
    bool open_ = false;
    CellPadInfo2 info_{};
    bool have_info_ = false;
    std::uint32_t connected_mask_ = 0;
    int port_ = -1;
    bool port_connected_ = false;
    PortCaps caps_{};
    std::uint32_t prior_setting_ = 0;
    bool setting_changed_ = false;
    bool press_on_ = false;
    bool sensor_on_ = false;
    bool motors_active_ = false;
    bool suppress_held_ = false;
    std::int32_t rejected_len_ = 0;
    const char *first_error_ = nullptr;
    std::int32_t first_error_code_ = 0;
    unsigned stop_failures_ = 0;
    unsigned restore_failures_ = 0;
};

}  // namespace ds3b
