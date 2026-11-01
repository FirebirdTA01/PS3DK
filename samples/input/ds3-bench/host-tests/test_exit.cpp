// SELECT+START and the sysutil exit request both end the session; exit wins
// over every other button in the same frame; finish writes zero; close ends
// the library exactly once. A failed read ends the session.
#include "fake_pad.h"

int main()
{
    Rig r;
    r.start();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(r.bench.pulse_active());
    // Exit plus R1 plus CROSS in one frame: exit only.
    const std::size_t acts = r.pad.acts.size();
    CHECK(!r.step(fake::record(CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_START,
                               CELL_PAD_CTRL_R1 | CELL_PAD_CTRL_CROSS)));
    CHECK(r.bench.end() == ds3b::End::exit_combo);
    CHECK(r.pad.acts.size() == acts);
    CHECK(!r.bench.step());  // an ended session stays ended
    r.bench.finish();
    CHECK(r.pad.acts.back().small == 0 && r.pad.acts.back().large == 0);
    r.lc.close();
    CHECK(r.pad.ends == 1);

    // SELECT, then START after releasing SELECT: no exit.
    Rig s;
    s.start();
    CHECK(s.step(fake::record(CELL_PAD_CTRL_SELECT)));
    CHECK(s.step(fake::record(CELL_PAD_CTRL_START)));
    CHECK(s.bench.end() == ds3b::End::running);

    // sysutil exit request.
    Rig q;
    q.start();
    q.bench.request_exit();
    CHECK(!q.step_none());
    CHECK(q.bench.end() == ds3b::End::exit_request);

    // A failed read ends the session as read_failed.
    Rig f;
    f.start();
    f.pad.data_rc = {static_cast<std::int32_t>(CELL_PAD_ERROR_DATA_READ_FAILED)};
    CHECK(!f.step_none());
    CHECK(f.bench.end() == ds3b::End::read_failed);
    return finish_checks("test_exit");
}
