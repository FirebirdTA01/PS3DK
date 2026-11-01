// Fake-clock deadlines: every pulse lasts 200 ms; a replacement pulse
// restarts the deadline; the session ends at 120 s (or a configured value).
#include "fake_pad.h"

int main()
{
    Rig r;
    r.start();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_SQUARE)));
    const std::uint64_t first = r.bench.pulse_deadline();
    r.advance_ms(150);
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));  // replaces at t=150 ms
    CHECK(r.bench.pulse_deadline() == first + 150000);
    CHECK(r.log.has("DS3B_PULSE replaced"));
    r.advance_ms(199);
    CHECK(r.step(fake::record()));
    CHECK(r.bench.pulse_active());
    r.advance_ms(1);
    CHECK(r.step(fake::record()));
    CHECK(!r.bench.pulse_active());
    CHECK(r.log.has("why=deadline elapsed_us=200000"));

    Rig s;
    s.start();
    s.advance_ms(119999);
    CHECK(s.step_none());
    s.advance_ms(1);
    CHECK(!s.step_none());
    CHECK(s.bench.end() == ds3b::End::session_deadline);

    ds3b::Config cfg;
    cfg.session_us = 5000000;
    Rig c(fake::kAll, cfg);
    c.start();
    c.advance_ms(5000);
    CHECK(!c.step_none());
    CHECK(c.bench.end() == ds3b::End::session_deadline);
    return finish_checks("test_deadlines");
}
