// CELL_OK with len 0 is success with no new data: counted, never an error,
// and pulse deadlines still fire while no records arrive.
#include "fake_pad.h"

int main()
{
    Rig r;
    r.start();
    const unsigned before = r.bench.counters().no_new_data;
    CHECK(r.step_none());
    CHECK(r.step_none());
    CHECK(r.bench.counters().no_new_data == before + 2);
    CHECK(r.bench.end() == ds3b::End::running);
    // Start a pulse, then only no-new-data reads until past the deadline.
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CROSS)));
    CHECK(r.bench.pulse_active());
    r.advance_ms(199);
    CHECK(r.step_none());
    CHECK(r.bench.pulse_active());
    r.advance_ms(1);
    CHECK(r.step_none());
    CHECK(!r.bench.pulse_active());
    CHECK(!r.pad.acts.empty() && r.pad.acts.back().small == 0 && r.pad.acts.back().large == 0);
    CHECK(r.bench.counters().pulses_stopped == 1);
    CHECK(r.bench.counters().malformed == 0);
    return finish_checks("test_no_new_data");
}
