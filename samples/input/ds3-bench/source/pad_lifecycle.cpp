#include "pad_lifecycle.h"

namespace ds3b {

namespace {
constexpr std::int32_t kNoDevice = static_cast<std::int32_t>(CELL_PAD_ERROR_NO_DEVICE);
}

const char *read_name(Read r)
{
    switch (r) {
    case Read::fresh: return "fresh";
    case Read::no_new_data: return "no_new_data";
    case Read::disconnected: return "disconnected";
    case Read::malformed: return "malformed";
    case Read::failed: return "failed";
    case Read::no_port: return "no_port";
    }
    return "unknown";
}

void PadLifecycle::note(const char *what, std::int32_t code)
{
    if (!first_error_) {
        first_error_ = what;
        first_error_code_ = code;
    }
}

std::int32_t PadLifecycle::open(std::uint32_t max_connect)
{
    if (open_)
        return CELL_OK;
    const std::int32_t rc = ops_.init(ops_.ctx, max_connect);
    if (rc == CELL_OK)
        open_ = true;
    else
        note("cellPadInit", rc);
    return rc;
}

std::int32_t PadLifecycle::refresh()
{
    if (!open_)
        return CELL_PAD_ERROR_UNINITIALIZED;
    CellPadInfo2 info{};
    const std::int32_t rc = ops_.get_info2(ops_.ctx, &info);
    if (rc != CELL_OK) {
        note("cellPadGetInfo2", rc);
        return rc;
    }
    info_ = info;
    have_info_ = true;
    connected_mask_ = 0;
    for (std::uint32_t p = 0; p < CELL_PAD_MAX_PORT_NUM; ++p)
        if (info_.port_status[p] & CELL_PAD_STATUS_CONNECTED)
            connected_mask_ |= 1u << p;
    if (selected()) {
        const bool now = (connected_mask_ >> port_) & 1u;
        if (port_connected_ && !now) {
            drop_connection();
        } else if (!port_connected_ && now) {
            acquire(static_cast<std::uint32_t>(port_));
        }
    }
    return CELL_OK;
}

// Nothing can be written to an absent controller. The device forgets its
// motor state; a reconnect starts from the reported port_setting again.
void PadLifecycle::drop_connection()
{
    port_connected_ = false;
    motors_active_ = false;
    setting_changed_ = false;
    press_on_ = sensor_on_ = false;
}

bool PadLifecycle::acquire(std::uint32_t port)
{
    port_ = static_cast<int>(port);
    port_connected_ = (connected_mask_ >> port) & 1u;
    caps_ = PortCaps{};
    setting_changed_ = false;
    press_on_ = sensor_on_ = false;
    motors_active_ = false;
    suppress_held_ = true;
    if (!port_connected_)
        return false;
    const std::uint32_t cap = info_.device_capability[port];
    caps_.raw = cap;
    caps_.press = cap & CELL_PAD_CAPABILITY_PRESS_MODE;
    caps_.sensor = cap & CELL_PAD_CAPABILITY_SENSOR_MODE;
    caps_.actuator = cap & CELL_PAD_CAPABILITY_ACTUATOR;
    prior_setting_ = info_.port_setting[port];
    std::uint32_t want = prior_setting_;
    if (wanted_.press && caps_.press)
        want |= CELL_PAD_SETTING_PRESS_ON;
    if (wanted_.sensor && caps_.sensor)
        want |= CELL_PAD_SETTING_SENSOR_ON;
    if (want != prior_setting_) {
        const std::int32_t rc = ops_.set_port_setting(ops_.ctx, port, want);
        if (rc == CELL_OK) {
            setting_changed_ = true;
        } else {
            note("cellPadSetPortSetting(enable)", rc);
            want = prior_setting_;
        }
    }
    press_on_ = want & CELL_PAD_SETTING_PRESS_ON;
    sensor_on_ = want & CELL_PAD_SETTING_SENSOR_ON;
    return true;
}

bool PadLifecycle::select(std::uint32_t port)
{
    if (!open_ || port >= CELL_PAD_MAX_PORT_NUM || !have_info_)
        return false;
    if (!((connected_mask_ >> port) & 1u))
        return false;
    if (!release())
        return false;
    return acquire(port);
}

bool PadLifecycle::select_next()
{
    if (!connected_mask_)
        return false;
    const int start = selected() ? port_ : -1;
    for (int step = 1; step <= static_cast<int>(CELL_PAD_MAX_PORT_NUM); ++step) {
        const int p = (start + step + static_cast<int>(CELL_PAD_MAX_PORT_NUM)) %
                      static_cast<int>(CELL_PAD_MAX_PORT_NUM);
        if ((connected_mask_ >> p) & 1u)
            return select(static_cast<std::uint32_t>(p));
    }
    return false;
}

bool PadLifecycle::restore_setting()
{
    if (!setting_changed_)
        return true;
    for (unsigned attempt = 0; attempt < kWriteAttempts; ++attempt) {
        const std::int32_t rc = ops_.set_port_setting(ops_.ctx, static_cast<std::uint32_t>(port_),
                                                      prior_setting_);
        if (rc == kNoDevice) {  // the controller left: it forgets the setting
            drop_connection();
            return true;
        }
        if (rc == CELL_OK) {
            setting_changed_ = false;
            press_on_ = prior_setting_ & CELL_PAD_SETTING_PRESS_ON;
            sensor_on_ = prior_setting_ & CELL_PAD_SETTING_SENSOR_ON;
            return true;
        }
        note("cellPadSetPortSetting(restore)", rc);
    }
    ++restore_failures_;
    return false;
}

bool PadLifecycle::release()
{
    if (!selected())
        return true;
    if (port_connected_) {
        bool ok = true;
        if (motors_active_ && !stop_motors())
            ok = false;
        if (!restore_setting())
            ok = false;
        if (!ok)
            return false;  // keep the port: its cleanup is still owed
    }
    port_ = -1;
    port_connected_ = false;
    caps_ = PortCaps{};
    setting_changed_ = false;
    press_on_ = sensor_on_ = false;
    motors_active_ = false;
    return true;
}

void PadLifecycle::close()
{
    if (!open_)
        return;
    release();
    port_ = -1;
    port_connected_ = false;
    const std::int32_t rc = ops_.end(ops_.ctx);
    if (rc != CELL_OK)
        note("cellPadEnd", rc);
    open_ = false;
}

Read PadLifecycle::read(CellPadData &out)
{
    if (!selected())
        return Read::no_port;
    if (!port_connected_)
        return Read::disconnected;
    CellPadData d{};
    const std::int32_t rc = ops_.get_data(ops_.ctx, static_cast<std::uint32_t>(port_), &d);
    if (rc == kNoDevice) {
        drop_connection();
        return Read::disconnected;
    }
    if (rc != CELL_OK) {
        note("cellPadGetData", rc);
        return Read::failed;
    }
    if (d.len == 0)
        return Read::no_new_data;
    if (d.len < kLenDigital || d.len > CELL_PAD_MAX_CODES) {
        rejected_len_ = d.len;
        return Read::malformed;
    }
    out = d;
    return Read::fresh;
}

bool PadLifecycle::set_motors(std::uint8_t small, std::uint8_t large)
{
    if (!port_connected() || !caps_.actuator || small > 1)
        return false;
    CellPadActParam p{};
    p.motor[0] = small;
    p.motor[1] = large;
    const std::int32_t rc = ops_.set_act_direct(ops_.ctx, static_cast<std::uint32_t>(port_), &p);
    if (rc == kNoDevice) {
        drop_connection();
        return false;
    }
    if (rc != CELL_OK) {
        note("cellPadSetActDirect", rc);
        return false;
    }
    motors_active_ = small || large;
    return true;
}

bool PadLifecycle::stop_motors()
{
    if (!port_connected() || !caps_.actuator) {
        motors_active_ = false;
        return true;
    }
    for (unsigned attempt = 0; attempt < kWriteAttempts; ++attempt) {
        CellPadActParam p{};
        const std::int32_t rc =
            ops_.set_act_direct(ops_.ctx, static_cast<std::uint32_t>(port_), &p);
        if (rc == CELL_OK) {
            motors_active_ = false;
            return true;
        }
        if (rc == kNoDevice) {  // the controller left: nothing left to stop
            drop_connection();
            return true;
        }
        note("cellPadSetActDirect(stop)", rc);
    }
    ++stop_failures_;
    return false;
}

}  // namespace ds3b
