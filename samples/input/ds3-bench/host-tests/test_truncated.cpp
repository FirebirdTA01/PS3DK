// Records shorter than a field must never be read past their length.
#include "fake_pad.h"

int main()
{
    Rig r;
    r.start(false);
    const unsigned reads = r.pad.reads;
    // 0 < len < 4: no digital field -> malformed, counted, session continues.
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CROSS, 3)));
    CHECK(r.bench.counters().malformed == 1);
    CHECK(r.pad.acts.empty());  // the CROSS bit beyond len was not acted on
    // Negative and over-long lengths are malformed too.
    CHECK(r.step(fake::record(0, 0, -1)));
    CHECK(r.step(fake::record(0, 0, CELL_PAD_MAX_CODES + 1)));
    CHECK(r.bench.counters().malformed == 3);
    // The reported length is the rejected record's own len.
    CHECK(r.log.has("malformed len=3"));
    CHECK(r.log.has("malformed len=-1"));
    CHECK(r.log.has("malformed len=65"));
    CHECK(!r.log.has("malformed len=0"));
    // len 4: digital only; no analog line even though sticks are set.
    CHECK(r.step(fake::record(0, 0, 4)));
    CHECK(r.bench.counters().fresh == 1);
    CHECK(r.bench.counters().analog_lines == 0);
    // len 8: sticks, but pressure (index 8..19) must not be read.
    auto rec = fake::record(0, 0, 8);
    rec.button[CELL_PAD_BTN_OFFSET_PRESS_CROSS] = 0xff;
    CHECK(r.step(rec));
    CHECK(r.bench.counters().analog_lines == 1);
    CHECK(r.log.has("press=off"));
    CHECK(!r.log.has("X=255"));
    CHECK(r.pad.reads == reads + 5);
    return finish_checks("test_truncated");
}
