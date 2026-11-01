// The on-screen page: controls are always listed; every button shows its
// state; sticks, pressure and Sixaxis appear only when the record holds them
// and the mode is on; cached values never survive a controller change;
// rumble shows the last command, not a measured motor; every row fits; and a
// stalled or failed display never blocks motor stops or exit.
#include "fake_pad.h"

static bool has_seg(const ds3b::Screen &s, const char *text, ds3b::Ink ink)
{
    for (const auto &r : s.rows)
        for (const auto &g : r.segments)
            if (g.ink == ink && g.text.find(text) != std::string::npos)
                return true;
    return false;
}

static bool fits(const ds3b::Screen &s)
{
    for (const auto &r : s.rows) {
        std::size_t n = 0;
        for (const auto &g : r.segments)
            n += g.text.size();
        if (n > ds3b::kScreenColumns) {
            std::fprintf(stderr, "row of %zu chars\n", n);
            return false;
        }
    }
    return true;
}

static bool contains(const ds3b::Screen &s, const char *text)
{
    return s.text().find(text) != std::string::npos;
}

// A rig with both pressure and Sixaxis requested.
struct SensorRig {
    fake::Pad pad;
    fake::Clock clk;
    fake::Log log;
    ds3b::PadLifecycle lc;
    ds3b::Bench bench;
    static ds3b::Wanted both()
    {
        ds3b::Wanted w;
        w.press = true;
        w.sensor = true;
        return w;
    }
    SensorRig() : lc(fake::ops(pad), both()), bench(lc, fake::clock(clk), fake::log(log))
    {
        pad.connect(0, fake::kAll);
        pad.connect(1, fake::kAll);
        lc.open();
        lc.refresh();
        bench.begin();
        step_none();
        step_none();
    }
    bool step(const CellPadData &d)
    {
        pad.records.push_back(d);
        lc.refresh();
        return bench.step();
    }
    bool step_none()
    {
        lc.refresh();
        return bench.step();
    }
};

static CellPadData sensor_record()
{
    auto m = fake::record();
    m.button[CELL_PAD_BTN_OFFSET_SENSOR_X] = 0x1ff;
    m.button[CELL_PAD_BTN_OFFSET_SENSOR_Y] = 0x200;
    m.button[CELL_PAD_BTN_OFFSET_SENSOR_Z] = 0x190;
    m.button[CELL_PAD_BTN_OFFSET_SENSOR_G] = 0xfdfe;  // only 10 bits are a value
    return m;
}

static void content()
{
    using ds3b::Ink;
    Rig r;  // pressure requested, Sixaxis not requested
    r.start();
    auto rec = fake::record(CELL_PAD_CTRL_UP, CELL_PAD_CTRL_TRIANGLE);
    rec.button[CELL_PAD_BTN_OFFSET_PRESS_TRIANGLE] = 77;
    CHECK(r.step(rec));
    const ds3b::Screen s = r.bench.screen();
    CHECK(contains(s, "SELECT+START exit"));
    CHECK(contains(s, "CROSS small"));
    CHECK(has_seg(s, "UP ", Ink::on));
    CHECK(has_seg(s, "DOWN ", Ink::dim));
    CHECK(has_seg(s, "TRIANGLE ", Ink::on));
    CHECK(has_seg(s, "CROSS ", Ink::dim));
    CHECK(contains(s, "Left stick  X 128  Y 128"));
    CHECK(has_seg(s, "TR  77", Ink::on));
    CHECK(contains(s, "Sixaxis: not requested by this build"));
    CHECK(contains(s, "pressure:ON"));
    CHECK(fits(s));

    // The rumble row reports the last command, then the stop.
    CHECK(r.step(fake::record()));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    CHECK(has_seg(r.bench.screen(), "last command: small off, large 255", Ink::on));
    r.advance_ms(200);
    CHECK(r.step(fake::record()));
    CHECK(has_seg(r.bench.screen(), "last command: stop", Ink::dim));
    CHECK(contains(r.bench.screen(), "time left 119 s"));
}

static void sixaxis()
{
    SensorRig g;
    CHECK(g.lc.sensor_active());
    CHECK(g.step(sensor_record()));
    const ds3b::Screen s = g.bench.screen();
    CHECK(contains(s, "accel X  511  Y  512  Z  400   G  510"));
    CHECK(contains(s, "one raw gyro channel"));
    CHECK(contains(s, "not verified on hardware"));
    CHECK(!contains(s, "512 at rest"));
    CHECK(fits(s));
    // Enabled, but the record is too short for the sensor fields.
    CHECK(g.step(fake::record(0, 0, 20)));
    CHECK(!contains(g.bench.screen(), "accel X"));
    CHECK(contains(g.bench.screen(), "Sixaxis: enabled, but not in this record (len 20)"));
}

static void failed_enable()
{
    // Sensor requested, the enable write fails: say it failed.
    fake::Pad p;
    p.connect(0, fake::kAll);
    p.setting_rc = {static_cast<std::int32_t>(CELL_PAD_ERROR_INVALID_PARAMETER)};
    ds3b::PadLifecycle lc(fake::ops(p), SensorRig::both());
    fake::Clock clk;
    fake::Log log;
    ds3b::Bench b(lc, fake::clock(clk), fake::log(log));
    lc.open();
    lc.refresh();
    b.begin();
    lc.refresh();
    b.step();
    p.records.push_back(sensor_record());
    lc.refresh();
    b.step();
    p.records.push_back(sensor_record());
    lc.refresh();
    b.step();
    CHECK(contains(b.screen(), "Sixaxis: not enabled (the enable request failed)"));
    CHECK(contains(b.screen(), "Pressure: not enabled (the enable request failed)"));
}

static void cache_cleared_on_switch_and_reconnect()
{
    SensorRig g;
    CHECK(g.step(fake::record(0, CELL_PAD_CTRL_TRIANGLE)));
    CHECK(contains(g.bench.screen(), "Left stick"));
    // L1 to port 1: cleared in the same iteration, before it would draw.
    CHECK(g.step(fake::record()));
    CHECK(g.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(g.lc.port() == 1);
    CHECK(contains(g.bench.screen(), "Waiting for the first record"));
    CHECK(!contains(g.bench.screen(), "Left stick"));
    // Then only len 0: still never port 0's old values.
    CHECK(g.step_none());
    CHECK(contains(g.bench.screen(), "Waiting for the first record"));
    CHECK(!contains(g.bench.screen(), "Left stick"));
    // A record on port 1 shows; then a disconnect and reconnect, then len 0.
    CHECK(g.step(fake::record(CELL_PAD_CTRL_DOWN)));
    CHECK(contains(g.bench.screen(), "Left stick"));
    g.pad.disconnect(1);
    CHECK(g.step_none());
    CHECK(contains(g.bench.screen(), "No controller"));
    g.pad.connect(1, fake::kAll);
    CHECK(g.step_none());
    CHECK(g.step_none());
    CHECK(contains(g.bench.screen(), "Waiting for the first record"));
    CHECK(!contains(g.bench.screen(), "DOWN"));
    // Ordinary len 0 on the same connection keeps the last values.
    CHECK(g.step(fake::record(CELL_PAD_CTRL_DOWN)));
    CHECK(g.step_none());
    CHECK(contains(g.bench.screen(), "Left stick"));

    // L1 with one controller re-acquires the same port: also cleared at once.
    Rig one;
    one.start();
    CHECK(one.step(fake::record(CELL_PAD_CTRL_DOWN)));
    CHECK(contains(one.bench.screen(), "Left stick"));
    CHECK(one.step(fake::record()));
    CHECK(one.step(fake::record(0, CELL_PAD_CTRL_L1)));
    CHECK(one.lc.port() == 0);
    CHECK(contains(one.bench.screen(), "Waiting for the first record"));
    CHECK(contains(one.bench.screen(), "no command sent to this controller yet"));
}

static void rumble_wording()
{
    Rig r;
    r.start();
    // A fresh acquire with no motor write is not a stop command.
    CHECK(contains(r.bench.screen(), "no command sent to this controller yet"));
    CHECK(!contains(r.bench.screen(), "last command: stop"));
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_R1)));
    CHECK(contains(r.bench.screen(), "last command: stop"));
}

static void warnings_fit_and_persist()
{
    ds3b::Config cfg;
    cfg.stop_retry_steps = 1;
    Rig r(fake::kAll, cfg);
    r.start();
    CHECK(r.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    r.pad.act_rc.assign(30, static_cast<std::int32_t>(CELL_PAD_ERROR_FATAL));
    r.advance_ms(200);
    CHECK(r.step_none());
    CHECK(has_seg(r.bench.screen(), "STOP FAILED: retrying", ds3b::Ink::warn));
    CHECK(r.step_none());
    CHECK(r.step_none());
    // Gave up: the warning stays even after other log lines replace "Last".
    for (int i = 0; i < 3; ++i)
        CHECK(r.step(fake::record(CELL_PAD_CTRL_UP)) && r.step(fake::record()));
    CHECK(has_seg(r.bench.screen(), "motors may still be running", ds3b::Ink::warn));
    CHECK(fits(r.bench.screen()));

    Rig n(CELL_PAD_CAPABILITY_PS3_CONFORMITY);
    n.start();
    CHECK(n.step(fake::record(0, CELL_PAD_CTRL_CROSS)));
    CHECK(has_seg(n.bench.screen(), "refused", ds3b::Ink::warn));
    CHECK(contains(n.bench.screen(), "Rumble   not supported"));
    CHECK(fits(n.bench.screen()));

    Rig d;
    d.start();
    CHECK(d.step(fake::record(0, CELL_PAD_CTRL_CIRCLE)));
    d.pad.disconnect(0);
    CHECK(d.step_none());
    CHECK(has_seg(d.bench.screen(), "No controller", ds3b::Ink::warn));
    CHECK(has_seg(d.bench.screen(), "unknown: controller disconnected", ds3b::Ink::warn));
    CHECK(fits(d.bench.screen()));
}

// A display whose flips never complete, and one that fails.
struct FakeDisplay {
    int ready_value = 0;
    int draw_value = 0;
    unsigned draws = 0;
};
static int fake_ready(void *c) { return static_cast<FakeDisplay *>(c)->ready_value; }
static int fake_draw(void *c, const ds3b::Screen &)
{
    auto &f = *static_cast<FakeDisplay *>(c);
    ++f.draws;
    return f.draw_value;
}

static void stalled_display_never_blocks()
{
    Rig r;
    r.start();
    FakeDisplay fd;  // ready() always 0: the previous flip never completes
    const ds3b::DisplayOps ops{fake_ready, fake_draw, &fd};
    ds3b::DisplayStats st;
    auto it = [&](const CellPadData &d) {
        r.pad.records.push_back(d);
        return ds3b::iterate(r.lc, r.bench, &ops, st);
    };
    // R1 during an active pulse.
    CHECK(it(fake::record(0, CELL_PAD_CTRL_CIRCLE)) == ds3b::Iteration::running);
    CHECK(r.bench.pulse_active());
    CHECK(it(fake::record(0, CELL_PAD_CTRL_R1)) == ds3b::Iteration::running);
    CHECK(!r.bench.pulse_active());
    CHECK(r.pad.acts.back().small == 0 && r.pad.acts.back().large == 0);
    // The 200 ms deadline stop.
    CHECK(it(fake::record()) == ds3b::Iteration::running);
    CHECK(it(fake::record(0, CELL_PAD_CTRL_CIRCLE)) == ds3b::Iteration::running);
    CHECK(r.bench.pulse_active());
    r.advance_ms(200);
    CHECK(it(fake::record(0, CELL_PAD_CTRL_CIRCLE)) == ds3b::Iteration::running);
    CHECK(!r.bench.pulse_active());
    CHECK(r.pad.acts.back().small == 0 && r.pad.acts.back().large == 0);
    // Exit.
    CHECK(it(fake::record()) == ds3b::Iteration::running);
    CHECK(it(fake::record(CELL_PAD_CTRL_SELECT | CELL_PAD_CTRL_START)) == ds3b::Iteration::ended);
    CHECK(fd.draws == 0);
    CHECK(st.skipped == 6 && st.drawn == 0 && !st.stopped);

    // A display that errors: it stops, the session keeps running.
    Rig e;
    e.start();
    FakeDisplay bad;
    bad.ready_value = 1;
    bad.draw_value = -3;
    const ds3b::DisplayOps eops{fake_ready, fake_draw, &bad};
    ds3b::DisplayStats est;
    CHECK(ds3b::iterate(e.lc, e.bench, &eops, est) == ds3b::Iteration::running);
    CHECK(est.stopped);
    CHECK(ds3b::iterate(e.lc, e.bench, &eops, est) == ds3b::Iteration::running);
    CHECK(bad.draws == 1);  // not retried after it stopped

    // No display at all.
    ds3b::DisplayStats nst;
    CHECK(ds3b::iterate(e.lc, e.bench, nullptr, nst) == ds3b::Iteration::running);
}

int main()
{
    content();
    sixaxis();
    failed_enable();
    cache_cleared_on_switch_and_reconnect();
    rumble_wording();
    warnings_fit_and_persist();
    stalled_display_never_blocks();
    return finish_checks("test_screen");
}
