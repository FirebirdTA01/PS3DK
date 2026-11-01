// Releasing a port restores its prior port_setting; a failed restore is
// retried a bounded number of times, counted and reported, and close still
// ends the library.
#include "fake_pad.h"

int main()
{
    Rig r;
    r.pad.connect(0, fake::kAll, CELL_PAD_SETTING_SENSOR_ON);  // prior setting has sensor on
    r.start();
    CHECK(r.lc.press_active());
    CHECK(r.pad.settings.size() == 1);
    CHECK(r.pad.settings[0].value == (CELL_PAD_SETTING_SENSOR_ON | CELL_PAD_SETTING_PRESS_ON));
    const std::int32_t bad = static_cast<std::int32_t>(CELL_PAD_ERROR_FATAL);
    r.pad.setting_rc = {bad, bad, bad};
    r.bench.finish();
    r.lc.close();
    CHECK(r.pad.settings.size() == 1 + ds3b::PadLifecycle::kWriteAttempts);
    CHECK(r.pad.settings.back().value == CELL_PAD_SETTING_SENSOR_ON);  // other bits preserved
    CHECK(r.lc.restore_failures() == 1);
    CHECK(r.lc.first_error() &&
          std::strcmp(r.lc.first_error(), "cellPadSetPortSetting(restore)") == 0);
    CHECK(r.pad.ends == 1);
    r.lc.close();  // idempotent
    CHECK(r.pad.ends == 1);

    // A successful restore: one enable write, one restore write.
    Rig ok;
    ok.start();
    ok.lc.close();
    CHECK(ok.pad.settings.size() == 2);
    CHECK(ok.pad.settings[1].value == 0);
    CHECK(ok.lc.restore_failures() == 0);
    return finish_checks("test_failed_restore");
}
