// Review cases: a NO_DEVICE read while CellPadInfo2 still says connected is a
// disconnect (re-acquire, held-button suppression, no writes to the absent
// port); a failed zero or failed restore refuses a port switch and the old
// port's cleanup is retried on exit, never moved to the new port.
#include "fake_pad.h"

static const std::int32_t kBad = static_cast<std::int32_t>(CELL_PAD_ERROR_FATAL);
static const std::int32_t kGone = static_cast<std::int32_t>(CELL_PAD_ERROR_NO_DEVICE);

static void no_device_read()
{
    Rig r;
    r.start();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.bench.pulse_active());
    const std::size_t acts = r.pad.acts.size();
    // Info2 keeps reporting port 0 connected; the read says NO_DEVICE.
    r.pad.data_rc = {kGone};
    CHECK(r.step_none());
    CHECK(!r.bench.pulse_active());
    CHECK(r.bench.counters().disconnects == 1);
    CHECK(r.log.has("source=read"));
    // The deadline passes while gone: no write to the absent port.
    r.advance_ms(300);
    // Data resumes with CIRCLE still held: re-acquired, held press ignored.
    const std::size_t settings = r.pad.settings.size();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.pad.settings.size() == settings + 1);  // pressure re-enabled on re-acquire
    CHECK(r.pad.acts.size() == acts);
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.pad.acts.size() == acts);
    CHECK(r.bench.counters().reconnects == 1);
    // Release, then a new press fires.
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.pad.acts.size() == acts + 1);
    CHECK(r.pad.acts.back().large == 255);
}

static void no_device_on_stop_write()
{
    // The deadline stop runs before the read in a step; if the controller
    // left, that write sees NO_DEVICE: a disconnect, not a stop failure.
    Rig r;
    r.start();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_SQUARE)));
    r.advance_ms(200);
    r.pad.act_rc = {kGone};
    CHECK(r.step_none());
    CHECK(!r.bench.pulse_active());
    CHECK(r.lc.stop_failures() == 0);
    CHECK(r.bench.counters().stop_failures == 0);
    CHECK(!r.lc.port_connected());
}

static void failed_zero_refuses_switch()
{
    ds3b::Config cfg;
    cfg.stop_retry_steps = 2;
    Rig r(fake::kAll, cfg);
    r.pad.connect(1, fake::kAll);
    r.start();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    r.pad.act_rc.assign(ds3b::PadLifecycle::kWriteAttempts, kBad);
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(r.lc.port() == 0);  // switch refused
    CHECK(r.log.has("next_port refused reason=cleanup_failed port=0"));
    CHECK(r.bench.counters().port_changes == 0);
    for (const auto &a : r.pad.acts)
        CHECK(a.port == 0);  // port 1 never written
    // The pending stop is retried on port 0 and now succeeds.
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(!r.lc.motors_active());
    CHECK(r.pad.acts.back().port == 0 && r.pad.acts.back().large == 0);
    // With cleanup done, a fresh L1 moves to port 1.
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(r.lc.port() == 1);
}

static void failed_zero_retried_on_exit()
{
    Rig r;
    r.pad.connect(1, fake::kAll);
    r.start();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    r.pad.act_rc.assign(ds3b::PadLifecycle::kWriteAttempts, kBad);
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(r.lc.port() == 0);
    r.bench.finish();  // the next write succeeds
    CHECK(r.pad.acts.back().port == 0 && r.pad.acts.back().large == 0);
    r.lc.close();
    for (const auto &a : r.pad.acts)
        CHECK(a.port == 0);
    for (const auto &s : r.pad.settings)
        CHECK(s.port == 0);
    CHECK(r.pad.ends == 1);
}

static void failed_restore_refuses_switch()
{
    Rig r;
    r.pad.connect(1, fake::kAll);
    r.start();
    r.pad.setting_rc.assign(ds3b::PadLifecycle::kWriteAttempts, kBad);
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(r.lc.port() == 0);
    CHECK(r.lc.restore_failures() == 1);
    for (const auto &s : r.pad.settings)
        CHECK(s.port == 0);  // port 1 not admitted
    // Exit retries the restore on port 0 (now succeeding), then ends.
    r.bench.finish();
    r.lc.close();
    CHECK(r.pad.settings.back().port == 0 && r.pad.settings.back().value == 0);
    CHECK(r.lc.restore_failures() == 1);
    CHECK(r.pad.ends == 1);
    for (const auto &s : r.pad.settings)
        CHECK(s.port == 0);
}

int main()
{
    no_device_read();
    no_device_on_stop_write();
    failed_zero_refuses_switch();
    failed_zero_retried_on_exit();
    failed_restore_refuses_switch();
    return finish_checks("test_switch_cleanup");
}
