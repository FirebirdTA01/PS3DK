//! The Frames page driven headlessly with egui input: a click on a slot goes through the real input handling.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use eframe::egui::{self, Event, Modifiers, PointerButton, Pos2, RawInput, Rect, Vec2};
use ps3dk_prof::{load, Analysis, Outcome};
use ps3dk_prof_viewer::frames::{slot_at, FramesIndex, Selection};
use ps3dk_prof_viewer::frames_ui::{draw, Drawn, FramesState};
use ps3dk_prof_viewer::timeline_ui::TimelineState;

fn analysed(bytes: &[u8]) -> Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

struct Harness {
    ctx: egui::Context,
    a: Analysis,
    ix: FramesIndex,
    st: FramesState,
    tl: TimelineState,
    time: f64,
    graph: Option<Rect>,
    drawn: Drawn,
}

impl Harness {
    fn new(a: Analysis) -> Harness {
        let ix = FramesIndex::build(&a);
        let mut st = FramesState::default();
        st.reset(Some(&ix));
        let tl = TimelineState::new(&a);
        let ctx = egui::Context::default();
        ps3dk_ui::install(&ctx); // the app's text styles (page headers, captions)
        let mut h = Harness { ctx, a, ix, st, tl, time: 0.0, graph: None, drawn: Drawn::default() };
        h.frame(vec![]);
        h
    }

    fn frame(&mut self, events: Vec<Event>) {
        self.time += 1.0 / 60.0;
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1200.0, 900.0))),
            time: Some(self.time),
            modifiers: Modifiers::NONE,
            events,
            ..Default::default()
        };
        let (a, ix, st, tl) = (&self.a, &self.ix, &mut self.st, &mut self.tl);
        let mut drawn = Drawn::default();
        let _ = self.ctx.run(input, |ctx| {
            egui::CentralPanel::default().show(ctx, |p| drawn = draw(p, a, ix, st, Some(tl)));
        });
        self.graph = drawn.graph;
        self.drawn = drawn;
    }

    /// Clicks the middle of slot `i` of `n` (near the baseline, where every glyph kind is drawn).
    fn click_slot(&mut self, i: usize, n: usize) {
        let r = self.graph.expect("a graph was drawn");
        let w = r.width() / n as f32;
        let at = Pos2::new(r.left() + w * (i as f32 + 0.5), r.bottom() - 8.0);
        let press = |pressed| Event::PointerButton { pos: at, button: PointerButton::Primary, pressed, modifiers: Modifiers::NONE };
        self.frame(vec![Event::PointerMoved(at)]);
        self.frame(vec![press(true)]);
        self.frame(vec![press(false)]);
        self.frame(vec![]);
    }
}

/// FRAMEs 0, 1, 2 at 1000, 4000, 9000 (frame 2 is final: UNKNOWN); one zone 1100..1400 so the Timeline has data.
fn capture() -> Analysis {
    let recs = seqd(
        0,
        [vec![frame(0, 1_000, 0)], zone(0, 0, 0, 1_100, 1, 0), vec![end(0, 0, 0, 1_400, 1), frame(0, 4_000, 1)], vec![
            frame(0, 9_000, 2),
        ]]
        .concat(),
    );
    analysed(&scenario(vec![lane(1, recs)]).bytes())
}

#[test]
fn clicking_a_measured_slot_selects_it_and_frames_the_timeline() {
    let mut h = Harness::new(capture());
    assert_eq!(h.st.sel, Selection { owner: Some(1), frame: None });
    let before = h.tl.view;
    h.click_slot(1, 3);
    assert_eq!(h.st.sel, Selection { owner: Some(1), frame: Some(1) });
    assert_ne!(h.tl.view, before, "a measured slot moves the Timeline");
    let (b, e) = (h.tl.tl.rel(4_000), h.tl.tl.rel(9_000));
    assert!(h.tl.view.t0 <= b && h.tl.view.t0 + h.tl.view.dt >= e, "framed with the margin: {:?}", h.tl.view);
    assert!(matches!(&h.st.tables, Some(((1, 1), _))));
}

#[test]
fn clicking_an_unknown_slot_selects_it_without_moving_the_timeline() {
    let mut h = Harness::new(capture());
    h.click_slot(0, 3); // measured: moves the Timeline
    let framed = h.tl.view;
    h.click_slot(2, 3); // the final frame: UNKNOWN
    assert_eq!(h.st.sel, Selection { owner: Some(1), frame: Some(2) });
    assert_eq!(h.tl.view, framed, "an unknown slot never moves the Timeline");
}

#[test]
fn no_owner_draws_no_graph() {
    let recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat());
    let h = Harness::new(analysed(&scenario(vec![lane(1, recs)]).bytes()));
    assert!(h.graph.is_none());
    assert_eq!(h.st.sel, Selection::default());
}

fn big(n: u64) -> Analysis {
    analysed(&chunked_frames(n))
}

#[test]
fn a_large_owner_paints_only_the_visible_columns_and_clicks_by_absolute_index() {
    let n = 100_001usize; // 100000 measured + the final frame
    let mut h = Harness::new(big(100_000));
    let r = h.graph.unwrap();
    let slot_w = r.width() / n as f32;
    let visible = (1200.0 / slot_w) as usize + 2;
    assert_eq!(h.st.painted.start, 0);
    assert!(h.st.painted.len() <= visible, "painted {:?} of {n}", h.st.painted);
    // Scroll far right with a horizontal wheel over the graph, over several frames (egui smooths the delta).
    let over = Pos2::new(600.0, r.center().y);
    h.frame(vec![Event::PointerMoved(over)]);
    for _ in 0..30 {
        h.frame(vec![Event::MouseWheel {
            unit: egui::MouseWheelUnit::Point,
            delta: Vec2::new(-1.0e9, 0.0),
            modifiers: Modifiers::NONE,
        }]);
    }
    for _ in 0..30 {
        h.frame(vec![]);
    }
    let r = h.graph.unwrap();
    assert!(r.left() < 0.0, "scrolled: {r:?}");
    assert_eq!(h.st.painted.end, n, "the last column is visible");
    assert!(h.st.painted.len() <= visible, "painted {:?}", h.st.painted);
    // A click maps to the absolute slot under the pointer, not to a viewport-relative one.
    let x = 900.0;
    let want = slot_at(n, f64::from(slot_w), f64::from(x - r.left())).unwrap();
    assert!(want > 90_000, "{want}");
    let at = Pos2::new(x, r.bottom() - 8.0);
    let press = |pressed| Event::PointerButton { pos: at, button: PointerButton::Primary, pressed, modifiers: Modifiers::NONE };
    h.frame(vec![Event::PointerMoved(at)]);
    h.frame(vec![press(true)]);
    h.frame(vec![press(false)]);
    assert_eq!(h.st.sel.frame, Some(want as u64), "frame ids equal slot indices in this capture");
}

#[test]
fn the_budget_is_drawn_only_with_a_clock() {
    let mut h = Harness::new(capture());
    assert!(h.drawn.budget_selector && h.drawn.budget_line);
    // A parsed capture always has CLOCKS before any record (chunk 1, freq_hz > 0), so frames without a clock exist
    // only in a torn file holding the HEADER alone: no frames, no graph, no budget.
    let mut f = scenario(vec![lane(1, seqd(0, vec![frame(0, 1, 0), frame(0, 2, 1)]))]).file();
    f.chunks.truncate(1);
    match load(&f.bytes()) {
        Outcome::Analysed(a) => {
            assert!(a.capture.clock.is_none());
            let t = Harness::new(*a);
            assert_eq!(t.drawn, Drawn::default());
        }
        Outcome::Malformed(m) => panic!("a torn header-only file is readable: {m}"),
    }
    // The index-level no-clock branch (defensive): no selector, no line, the page still draws its slots.
    h.ix.freq_hz = None;
    h.frame(vec![]);
    assert!(!h.drawn.budget_selector && !h.drawn.budget_line && h.drawn.graph.is_some());
}

/// One lane whose FRAME records are split over several RECORDS chunks (a single chunk would exceed the reader's
/// chunk length cap), as a real writer emits them.
fn chunked_frames(n: u64) -> Vec<u8> {
    let tbs: Vec<u64> = (0..=n).map(|i| 1_000 * i).collect();
    let recs = seqd(0, tbs.iter().enumerate().map(|(i, &tb)| frame(0, tb, i as u64)).collect());
    let s = scenario(vec![lane(1, recs.clone())]);
    let mut f = s.file();
    let at = f.find(T_RECORDS);
    f.chunks.remove(at);
    for (j, part) in recs.chunks(10_000).enumerate() {
        f.chunks.insert(at + j, chunk(T_RECORDS, records_payload(1, part)));
    }
    f.bytes()
}
