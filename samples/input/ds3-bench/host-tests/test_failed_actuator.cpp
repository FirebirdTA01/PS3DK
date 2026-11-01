// A failed motor write refuses the pulse; a failed stop is retried for a
// bounded number of steps, counted, and the first error is kept.
#include "fake_pad.h"

int main()
{
    ds3b::Config cfg;
    cfg.stop_retry_steps = 2;
    Rig r(fake::kAll, cfg);
    r.start();
    const std::int32_t bad = static_cast<std::int32_t>(CELL_PAD_ERROR_FATAL);
    r.pad.act_rc = {bad};
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.bench.counters().pulses_refused == 1);
    CHECK(!r.bench.pulse_active());
    CHECK(r.lc.first_error() && std::strcmp(r.lc.first_error(), "cellPadSetActDirect") == 0);
    // A good start, then every stop write fails.
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.bench.pulse_active());
    r.pad.act_rc.assign(30, bad);
    r.advance_ms(200);
    const std::size_t acts = r.pad.acts.size();
    for (int i = 0; i < 6; ++i)
        CHECK(r.step_none());
    // The deadline step plus 2 retry steps, each a bounded stop, then gave up.
    CHECK(r.pad.acts.size() == acts + 3 * ds3b::PadLifecycle::kWriteAttempts);
    CHECK(r.bench.counters().stop_failures == 3);
    CHECK(r.log.has("stop_gave_up"));
    CHECK(r.lc.stop_failures() == 3);
    // The first error is still the original start failure.
    CHECK(std::strcmp(r.lc.first_error(), "cellPadSetActDirect") == 0);
    return finish_checks("test_failed_actuator");
}
