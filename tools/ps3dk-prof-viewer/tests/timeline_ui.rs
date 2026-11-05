//! The timeline page driven headlessly with egui input events: wheel, drag,
//! shift+drag, double-click and click go through the real input handling.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use eframe::egui::{
    self, Event, Modifiers, MouseWheelUnit, PointerButton, Pos2, RawInput, Rect, Vec2,
};
use ps3dk_prof::{load, Analysis, Outcome};
use ps3dk_prof_viewer::model::ZoneKey;
use ps3dk_prof_viewer::timeline::{layout, DrawKind, View};
use ps3dk_prof_viewer::timeline_ui::{draw, TimelineState, ROW_H, TRACK_PAD};

fn analysed(bytes: &[u8]) -> Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

/// `lanes` lanes, each with zoneA 1000..5000 holding zoneB 2000..3000, and
/// FRAMEs at 0 and 10000.
fn capture(lanes: u32) -> Analysis {
    let mut ls = Vec::new();
    for inc in 1..=lanes {
        let recs = seqd(
            0,
            [
                vec![frame(0, 0, 0)],
                zone(0, 0, 0, 1000, 1, 0),
                zone(0, 1, 1, 2000, 2, 1),
                vec![end(0, 1, 1, 3000, 2), end(0, 0, 0, 5000, 1)],
                vec![frame(0, 10000, 1)],
            ]
            .concat(),
        );
        ls.push(lane(inc, recs));
    }
    analysed(&scenario(ls).bytes())
}

struct Harness {
    ctx: egui::Context,
    a: Analysis,
    st: TimelineState,
    selected: Option<ZoneKey>,
    time: f64,
    modifiers: Modifiers,
    slot: ps3dk_prof_viewer::timeline_foreign::ForeignTimelineSlot,
}

impl Harness {
    fn new(a: Analysis) -> Harness {
        let st = TimelineState::new(&a);
        let mut h = Harness {
            ctx: egui::Context::default(),
            a,
            st,
            selected: None,
            time: 0.0,
            modifiers: Modifiers::NONE,
            slot: Default::default(),
        };
        h.frame(vec![]);
        h
    }

    fn frame(&mut self, events: Vec<Event>) {
        self.time += 1.0 / 60.0;
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1200.0, 480.0))),
            time: Some(self.time),
            modifiers: self.modifiers,
            events,
            ..Default::default()
        };
        let (a, st, sel) = (&self.a, &mut self.st, &mut self.selected);
        let slot = &self.slot;
        let _ = self.ctx.run(input, |ctx| draw(ctx, slot, a, st, sel));
    }

    fn idle(&mut self, frames: usize) {
        for _ in 0..frames {
            self.frame(vec![]);
        }
    }

    fn area(&self) -> Rect {
        self.st.track_areas[0]
    }

    /// The point `fraction` across track 0's bar area, at bar row `depth`.
    fn at(&self, fraction: f32, depth: u8) -> Pos2 {
        let r = self.area();
        Pos2::new(
            r.left() + fraction * r.width(),
            r.top() + TRACK_PAD + depth as f32 * ROW_H + ROW_H / 2.0,
        )
    }

    fn press(&mut self, pos: Pos2, pressed: bool) {
        self.frame(vec![
            Event::PointerMoved(pos),
            Event::PointerButton {
                pos,
                button: PointerButton::Primary,
                pressed,
                modifiers: self.modifiers,
            },
        ]);
    }

    fn drag(&mut self, from: Pos2, to: Pos2) {
        self.press(from, true);
        for i in 1..=10 {
            let p = from + (to - from) * (i as f32 / 10.0);
            self.frame(vec![Event::PointerMoved(p)]);
        }
        self.press(to, false);
        self.idle(2);
    }

    fn click(&mut self, pos: Pos2) {
        self.press(pos, true);
        self.press(pos, false);
        self.idle(1);
    }
}

#[test]
fn wheel_zooms_at_the_pointer_and_does_not_scroll_the_lanes() {
    // Eight lanes (the reader's live limit) overflow the window, so the
    // lane list could scroll.
    let mut h = Harness::new(capture(8));
    assert_eq!(h.st.track_areas.len(), 8);
    assert!(
        h.st.track_areas[7].bottom() > 480.0,
        "the lanes overflow the window"
    );
    let top_before = h.area().top();
    let p = h.at(0.25, 0);
    h.frame(vec![Event::PointerMoved(p)]);
    let anchor_before =
        h.st.view
            .at(0.25 * h.area().width() as f64, h.area().width() as f64);
    h.frame(vec![Event::MouseWheel {
        unit: MouseWheelUnit::Point,
        delta: Vec2::new(0.0, 120.0),
        modifiers: Modifiers::NONE,
    }]);
    h.idle(30);
    let full = View::fit(&h.st.tl);
    assert!(h.st.view.dt < full.dt * 0.95, "zoomed in: {:?}", h.st.view);
    let w = h.area().width() as f64;
    let anchor_after = h.st.view.at(0.25 * w, w);
    assert!(
        (anchor_after - anchor_before).abs() < full.dt * 0.01,
        "the tick under the pointer stays put: {anchor_before} -> {anchor_after}"
    );
    assert_eq!(h.area().top(), top_before, "the lane list did not scroll");
}

#[test]
fn drag_pans_and_double_click_fits() {
    let mut h = Harness::new(capture(1));
    h.st.view.zoom(&h.st.tl, 0.5, 0.25);
    let before = h.st.view;
    let w = h.area().width();
    let (from, to) = (h.at(0.6, 0), h.at(0.4, 0));
    h.drag(from, to);
    let moved = h.st.view.t0 - before.t0;
    let expected = 0.2 * before.dt;
    assert!(
        (moved - expected).abs() < expected * 0.01,
        "drag left by a fifth pans right by a fifth: {moved} vs {expected} (w {w})"
    );
    assert_eq!(h.st.view.dt, before.dt, "pan keeps the zoom");
    let p = h.at(0.5, 0);
    h.click(p);
    h.click(p);
    assert_eq!(h.st.view, View::fit(&h.st.tl), "double-click fits");
}

#[test]
fn shift_drag_zooms_to_the_box_in_either_direction() {
    for (a, b) in [(0.2f32, 0.6f32), (0.6, 0.2)] {
        let mut h = Harness::new(capture(1));
        let fit = h.st.view;
        h.modifiers = Modifiers::SHIFT;
        let (from, to) = (h.at(a, 0), h.at(b, 0));
        let w = h.area().width() as f64;
        let (fa, fb) = (
            f64::from(from.x - h.area().left()) / w,
            f64::from(to.x - h.area().left()) / w,
        );
        h.drag(from, to);
        let mut want = fit;
        want.select(&h.st.tl, fa, fb);
        assert!(
            (h.st.view.t0 - want.t0).abs() < 1e-6 && (h.st.view.dt - want.dt).abs() < 1e-6,
            "box {a}->{b}: got {:?}, want {want:?}",
            h.st.view
        );
    }
}

#[test]
fn click_selects_a_zone_and_clicking_again_clears_it() {
    let mut h = Harness::new(capture(1));
    let w = h.area().width() as f64;
    let draws = layout(&h.st.tl, &h.st.tl.tracks[0], &h.st.view, w);
    let child = draws
        .iter()
        .find(|d| d.depth == 1 && matches!(d.kind, DrawKind::Bar(_)))
        .unwrap();
    let mid = ((child.x0 + child.x1) / 2.0 / w) as f32;
    let p = h.at(mid, 1);
    h.click(p);
    assert_eq!(h.selected, Some(("zoneB".to_string(), String::new())));
    h.idle(20); // past the double-click window
    h.click(p);
    assert_eq!(h.selected, None);
    // A selection made elsewhere (the Zones page) is the same value.
    h.selected = Some(("zoneA".to_string(), String::new()));
    h.idle(1);
    assert_eq!(h.selected, Some(("zoneA".to_string(), String::new())));
}

#[test]
fn a_new_file_starts_fitted() {
    let mut h = Harness::new(capture(1));
    h.st.view.zoom(&h.st.tl, 0.5, 0.1);
    let other = capture(2);
    h.st = TimelineState::new(&other);
    h.a = other;
    h.idle(1);
    assert_eq!(h.st.view, View::fit(&h.st.tl));
    assert_eq!(h.st.track_areas.len(), 2);
}

/// One lane with `zones` zoneA instances 1000 ticks apart: a capture that
/// "grows" as `zones` increases.
fn growing(zones: u64) -> Analysis {
    let mut parts = vec![vec![frame(0, 0, 0)]];
    for i in 0..zones {
        parts.push(zone(0, 0, 0, 1000 + i * 1000, i + 1, 0));
        parts.push(vec![end(0, 0, 0, 1500 + i * 1000, i + 1)]);
    }
    analysed(&scenario(vec![lane(1, seqd(0, parts.concat()))]).bytes())
}

#[test]
fn a_fitted_view_refits_as_the_capture_grows() {
    let mut st = TimelineState::new(&growing(2));
    assert!(st.fit_all);
    st.refresh(&growing(50));
    assert_eq!(st.view, View::fit(&st.tl), "still the whole capture");
}

#[test]
fn follow_keeps_the_width_and_moves_to_the_newest_data() {
    let mut st = TimelineState::new(&growing(10));
    st.view.zoom(&st.tl, 1.0, 0.25);
    st.fit_all = false;
    st.follow = true;
    let dt = st.view.dt;
    st.refresh(&growing(50));
    assert_eq!(st.view.dt, dt);
    assert!((st.view.t0 + st.view.dt - st.tl.length()).abs() < 1e-6);
}

#[test]
fn a_zoomed_view_keeps_its_window_as_the_capture_grows() {
    let mut st = TimelineState::new(&growing(10));
    st.view.zoom(&st.tl, 0.0, 0.25);
    st.fit_all = false;
    let before = st.view;
    st.refresh(&growing(50));
    assert_eq!(st.view, before);
}

#[test]
fn user_zoom_ends_follow_and_fit_all() {
    let mut h = Harness::new(capture(1));
    h.st.live = true;
    h.st.follow = true;
    assert!(h.st.fit_all);
    let p = h.at(0.5, 0);
    h.frame(vec![Event::PointerMoved(p)]);
    h.frame(vec![Event::MouseWheel {
        unit: MouseWheelUnit::Point,
        delta: Vec2::new(0.0, 120.0),
        modifiers: Modifiers::NONE,
    }]);
    h.idle(30);
    assert!(!h.st.follow && !h.st.fit_all);
    // Double-click fits again, which is fit_all.
    h.click(p);
    h.click(p);
    assert!(h.st.fit_all && !h.st.follow);
}

#[test]
fn a_box_in_progress_survives_a_refresh_unless_the_window_moves() {
    for (fit_all, kept) in [(false, true), (true, false)] {
        let mut h = Harness::new(growing(10));
        h.st.fit_all = fit_all;
        h.modifiers = Modifiers::SHIFT;
        let (from, to) = (h.at(0.2, 0), h.at(0.4, 0));
        h.press(from, true);
        for i in 1..=5 {
            let p = from + (to - from) * (i as f32 / 5.0);
            h.frame(vec![Event::PointerMoved(p)]);
        }
        assert!(h.st.box_active());
        let bigger = growing(50);
        h.st.refresh(&bigger);
        h.a = bigger;
        assert_eq!(h.st.box_active(), kept, "fit_all {fit_all}");
        h.press(to, false);
        h.idle(2);
        assert!(!h.st.box_active());
    }
}
