// Buttons held when a port is acquired are ignored until released; a held
// button never retriggers; each fresh press fires exactly once.
#include "fake_pad.h"

int main()
{
    Rig r;
    r.start(false);
    // First record after acquire: CROSS already held, so it is ignored.
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CROSS)));
    CHECK(r.pad.acts.empty());
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CROSS)));
    CHECK(r.pad.acts.empty());
    // Release, then press: one pulse.
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CROSS)));
    CHECK(r.pad.acts.size() == 1);
    CHECK(r.pad.acts[0].small == 1 && r.pad.acts[0].large == 0);
    // Keep holding across the deadline: stopped, not restarted.
    r.advance_ms(250);
    for (int i = 0; i < 5; ++i)
        CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CROSS)));
    CHECK(r.bench.counters().pulses_started == 1);
    CHECK(r.pad.acts.size() == 2 && r.pad.acts[1].small == 0 && r.pad.acts[1].large == 0);

    // Mapping of every pulse button (small motor on/off only).
    struct Case {
        std::uint8_t bit, small, large;
    };
    const Case cases[] = {{CELL_PAD_CTRL_CROSS, 1, 0},
                          {CELL_PAD_CTRL_SQUARE, 0, 64},
                          {CELL_PAD_CTRL_TRIANGLE, 1, 128},
                          {CELL_PAD_CTRL_CIRCLE, 0, 255}};
    for (const Case &c : cases) {
        CHECK(r.step(fake::record()));
        CHECK(r.step(fake::record(0, c.bit)));
        CHECK(r.pad.acts.back().small == c.small && r.pad.acts.back().large == c.large);
        CHECK(r.pad.acts.back().small <= 1);
    }
    // R1 writes zero at once and wins over a pulse pressed in the same frame.
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_R1 | CELL_PAD_CTRL_SQUARE)));
    CHECK(!r.bench.pulse_active());
    CHECK(r.pad.acts.back().small == 0 && r.pad.acts.back().large == 0);
    return finish_checks("test_held_buttons");
}
