// Analog and pressure lines: printed only when a value moves more than the
// 2-unit deadband from the last DISPLAYED value, at most 10 per second.
// Digital changes are always printed.
#include "fake_pad.h"

static CellPadData with(std::uint8_t lx, std::uint8_t press_x)
{
    auto d = fake::record();
    d.button[CELL_PAD_BTN_OFFSET_ANALOG_LEFT_X] = lx;
    d.button[CELL_PAD_BTN_OFFSET_PRESS_CROSS] = press_x;
    return d;
}

int main()
{
    Rig r;
    r.start(false);
    CHECK(r.step(with(0x80, 0)));
    CHECK(r.bench.counters().analog_lines == 1);  // the first record is always shown
    r.advance_ms(200);
    CHECK(r.step(with(0x82, 0)));  // +2: inside the deadband
    CHECK(r.bench.counters().analog_lines == 1);
    r.advance_ms(200);
    CHECK(r.step(with(0x83, 0)));  // +3 from the last displayed value
    CHECK(r.bench.counters().analog_lines == 2);
    // Slow drift is measured against the displayed value, not the last record.
    r.advance_ms(200);
    CHECK(r.step(with(0x84, 0)));
    CHECK(r.step(with(0x85, 0)));
    CHECK(r.bench.counters().analog_lines == 2);
    CHECK(r.step(with(0x86, 0)));
    CHECK(r.bench.counters().analog_lines == 3);
    // Rate limit: a big change 50 ms after a print waits for the interval.
    r.advance_ms(50);
    CHECK(r.step(with(0x10, 0)));
    CHECK(r.bench.counters().analog_lines == 3);
    r.advance_ms(50);
    CHECK(r.step(with(0x10, 0)));
    CHECK(r.bench.counters().analog_lines == 4);
    // Pressure counts as an analog value.
    r.advance_ms(200);
    CHECK(r.step(with(0x10, 200)));
    CHECK(r.bench.counters().analog_lines == 5);
    CHECK(r.log.has("X=200"));
    // Digital lines are not rate limited.
    const unsigned digital = r.bench.counters().digital_lines;
    CHECK(r.step(fake::record(CELL_PAD_CTRL_UP)));
    CHECK(r.step(fake::record(0)));
    CHECK(r.bench.counters().digital_lines == digital + 2);
    return finish_checks("test_reporter");
}
