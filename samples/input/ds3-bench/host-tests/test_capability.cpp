// Absent actuator: pulses refused with no motor writes. Absent pressure: no
// port_setting change. A failed PRESS_ON enable leaves pressure off, is
// recorded, and nothing is restored on release. A failed cellPadInit means
// close does not call cellPadEnd.
#include "fake_pad.h"

int main()
{
    Rig noact(CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_PRESS_MODE);
    noact.start();
    CHECK(noact.step(fake::record(0, CELL_PAD_CTRL_CROSS)));
    CHECK(noact.pad.acts.empty());
    CHECK(noact.bench.counters().pulses_refused == 1);
    CHECK(noact.log.has("no_actuator_capability"));
    CHECK(noact.step(fake::record(0, CELL_PAD_CTRL_R1)));
    CHECK(noact.pad.acts.empty());  // stop without an actuator writes nothing
    noact.bench.finish();
    noact.lc.close();
    CHECK(noact.pad.acts.empty());

    Rig nopress(CELL_PAD_CAPABILITY_PS3_CONFORMITY | CELL_PAD_CAPABILITY_ACTUATOR);
    nopress.start();
    CHECK(nopress.pad.settings.empty());
    CHECK(!nopress.lc.press_active());
    nopress.lc.close();
    CHECK(nopress.pad.settings.empty());

    Rig fail;
    fail.pad.setting_rc = {static_cast<std::int32_t>(CELL_PAD_ERROR_INVALID_PARAMETER)};
    fail.start();
    CHECK(fail.pad.settings.size() == 1);
    CHECK(!fail.lc.press_active());
    CHECK(fail.lc.first_error() &&
          std::strcmp(fail.lc.first_error(), "cellPadSetPortSetting(enable)") == 0);
    fail.lc.close();
    CHECK(fail.pad.settings.size() == 1);  // nothing to restore
    CHECK(fail.lc.restore_failures() == 0);

    fake::Pad p;
    p.init_rc = static_cast<std::int32_t>(CELL_PAD_ERROR_FATAL);
    ds3b::PadLifecycle lc(fake::ops(p), Rig::wanted());
    CHECK(lc.open() != CELL_OK);
    lc.close();
    CHECK(p.ends == 0);
    return finish_checks("test_capability");
}
