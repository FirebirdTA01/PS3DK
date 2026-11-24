// Disconnect drops the pulse without writing to an absent port; reconnect
// re-acquires (capabilities, pressure) and suppresses buttons already held.
// Switching ports writes zero to the old port and restores its setting first.
#include "fake_pad.h"

int main()
{
    Rig r;
    r.pad.connect(1, fake::kAll);
    r.start();
    CHECK(r.lc.port() == 0);
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_TRIANGLE)));
    CHECK(r.bench.pulse_active());
    CHECK(r.pad.acts.back().small == 1 && r.pad.acts.back().large == 128);

    r.pad.disconnect(0);
    const std::size_t acts = r.pad.acts.size();
    CHECK(r.step_none());
    CHECK(!r.bench.pulse_active());
    CHECK(r.bench.counters().disconnects == 1);
    r.advance_ms(500);
    CHECK(r.step_none());
    CHECK(r.pad.acts.size() == acts);  // nothing written to the absent port

    r.pad.connect(0, fake::kAll);
    const std::size_t settings = r.pad.settings.size();
    CHECK(r.step_none());  // refresh re-acquires; this step logs it
    CHECK(r.bench.counters().reconnects == 1);
    CHECK(r.pad.settings.size() == settings + 1);  // pressure enabled again
    CHECK(r.lc.press_active());
    // TRIANGLE still held from before the disconnect: no new pulse.
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_TRIANGLE)));
    CHECK(r.pad.acts.size() == acts);

    // L1 moves to port 1, writing zero to port 0 first and restoring it.
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.bench.pulse_active());
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(r.lc.port() == 1);
    CHECK(!r.bench.pulse_active());
    bool zero_on_0 = false, restore_on_0 = false;
    for (const auto &a : r.pad.acts)
        if (a.port == 0 && a.small == 0 && a.large == 0)
            zero_on_0 = true;
    for (const auto &s : r.pad.settings)
        if (s.port == 0 && s.value == 0)
            restore_on_0 = true;
    CHECK(zero_on_0);
    CHECK(restore_on_0);
    CHECK(r.bench.counters().port_changes == 1);
    return finish_checks("test_disconnect");
}
