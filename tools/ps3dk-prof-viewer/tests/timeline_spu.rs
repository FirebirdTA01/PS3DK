//! SPU ticks rows of the Timeline (spu-ticks slice design 9.4, delta D1.6, control C1-10), on captures from the
//! reader tests' writer.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;
#[path = "../../ps3dk-prof/tests/spu_support/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod spu_support;

use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use ps3dk_prof::foreign_timeline::{FtSourceState, TimelineBudgets};
use ps3dk_prof::spu_ticks::{LaneState, Options, PairOut, Place};
use ps3dk_prof_viewer::model::Document;
use ps3dk_prof_viewer::timeline::{timeline, View};
use ps3dk_prof_viewer::timeline_foreign::{build_track_axis, layout, omission_text, QueryCaps, ViewBudgets};
use ps3dk_prof_viewer::timeline_spu::*;
use ps3dk_prof_viewer::timeline_ui::TimelineState;
use spu_support::*;

/// 2 lanes x 2 epochs, 8 work pairs per epoch, 2 brackets per epoch (as the reader's F-BASE).
fn f_base() -> Cap {
    f_base_with(true)
}

/// F-BASE; `finish` false leaves out every ORDER_FINAL and DOMAIN_FINAL (a capture that is still arriving).
fn f_base_with(finish: bool) -> Cap {
    let mut c = Cap::new(2, 1000);
    for l in 1..=2 {
        let mut tb = 2000u64;
        c.epoch(l, 0, tb);
        let mut raw = 0xF000_0000u32;
        for e in 1..=2 {
            let mut items = Vec::new();
            for p in 0..8 {
                items.push(Item::Ev(raw, D_WORK, BEGIN, 0));
                raw -= 100;
                items.push(Item::Ev(raw, D_WORK, END, 0));
                raw -= 100;
                if p % 4 == 3 {
                    items.push(Item::Cal(raw - 50));
                    raw -= 100;
                }
            }
            let mut walks: Vec<Vec<Item>> = items.chunks(4).map(|w| w.to_vec()).collect();
            if e < 2 {
                walks.last_mut().unwrap().push(Item::Epoch(0xFFFF_FFFF));
            }
            for w in walks {
                if w.iter().any(|i| matches!(i, Item::Cal(_))) {
                    c.request(l, tb + 10);
                }
                tb += 1000;
                c.walk(l, tb, &w);
                if w.iter().any(|i| matches!(i, Item::Epoch(_))) {
                    raw = 0xFFFF_0000;
                    tb += 10;
                }
            }
        }
        if finish {
            c.finish(l);
        }
    }
    c
}

fn doc(bytes: &[u8]) -> Document {
    Document::from_bytes(PathBuf::from("t.prof"), bytes)
}

fn dump(name: &str, bytes: &[u8]) {
    if let Ok(dir) = std::env::var("SPU_VIEW_FIXTURE_DIR") {
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(format!("{dir}/{name}.prof"), bytes).unwrap();
    }
}

#[test]
fn c1_10_analysis_builds_once_per_document() {
    let b = f_base().bytes();
    dump("viewer-f-base", &b);
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let mut st = TimelineState::new(a);
    for _ in 0..100 {
        st.sync_foreign(&d.foreign_tl, a); // what draw() does first, every frame
    }
    assert_eq!(d.foreign_tl.spu.builds.load(Ordering::Relaxed), 1);
    // the isolation-scale option is formatting only; the W32 toggle rebuilds the rows but not the analysis
    st.spu_iso = true;
    st.w32 = true;
    for _ in 0..100 {
        st.sync_foreign(&d.foreign_tl, a);
    }
    st.spu_iso = false;
    st.w32 = false;
    st.sync_foreign(&d.foreign_tl, a);
    assert_eq!(d.foreign_tl.spu.builds.load(Ordering::Relaxed), 1);
    // SPU zones appear without the W32 assumption
    let fv = st.foreign.clone().unwrap();
    let spu: Vec<_> = fv.tracks.iter().filter(|t| t.spu.is_some()).collect();
    assert_eq!(spu.len(), 2);
    for t in &spu {
        assert_eq!(t.bars.len(), 16, "{}", t.header);
        assert!(t.refusal.is_none());
        assert!(t.header.contains("| placed 16/16 pairs | UNPLACED 0 | median width "), "{}", t.header);
        assert!(t.bars.iter().all(|b| !b.bar.extrapolated));
    }
    assert!(fv.caption.contains(CAPTION));
}

#[test]
fn c1_10_unsupported_result_shows_the_refusal_and_never_rebuilds() {
    let b = f_base().bytes();
    let mut d = doc(&b);
    d.foreign_tl.spu = SpuSlot::with_options(Options { capture_ops: 50, ..Options::default() });
    let a = d.analysis().unwrap();
    let mut st = TimelineState::new(a);
    for _ in 0..10 {
        st.sync_foreign(&d.foreign_tl, a);
    }
    st.w32 = true;
    st.sync_foreign(&d.foreign_tl, a);
    assert_eq!(d.foreign_tl.spu.builds.load(Ordering::Relaxed), 1);
    let v = d.foreign_tl.spu.get(a);
    let r = v.result.as_ref().unwrap();
    assert!(r.exhausted_at.is_some());
    let fv = st.foreign.clone().unwrap();
    let n = fv.notice.as_deref().unwrap_or("");
    assert!(n.contains("SPU ticks analysis: capture budget exhausted at"), "{n}");
    // the exhausted capture lists no lane, or lists it as UNSUPPORTED-RESOURCE: either way nothing is drawn
    for t in fv.tracks.iter().filter(|t| matches!(t.source, ps3dk_prof::ext::SourceKey::Lane(_))) {
        assert!(t.bars.is_empty());
    }
    // a lane listed as UNSUPPORTED-RESOURCE carries the refusal text
    let mut d2 = doc(&b);
    let full = Options::default();
    let used = ps3dk_prof::spu_ticks::analyze(d2.analysis().unwrap(), &full).unwrap().used;
    d2.foreign_tl.spu = SpuSlot::with_options(Options { capture_ops: used - 1, ..Options::default() });
    let a2 = d2.analysis().unwrap();
    let mut st2 = TimelineState::new(a2);
    st2.sync_foreign(&d2.foreign_tl, a2);
    st2.sync_foreign(&d2.foreign_tl, a2);
    assert_eq!(d2.foreign_tl.spu.builds.load(Ordering::Relaxed), 1);
    let fv2 = st2.foreign.clone().unwrap();
    let lane2 = fv2.tracks.iter().find(|t| t.label == "SPU lane 2").unwrap();
    assert!(lane2.bars.is_empty());
    let r = lane2.refusal.as_deref().unwrap();
    assert!(r.contains("UNSUPPORTED-RESOURCE (capture budget in publication)"), "{r}");
}

/// A result whose lane 1 holds `n` copies of a template pair, `place` applied to each copy.
fn synthetic(n: usize, place: &dyn Fn(usize, &mut PairOut)) -> Arc<SpuView> {
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let v = d.foreign_tl.spu.get(a);
    let mut v = (*v).clone();
    let r = v.result.as_mut().unwrap();
    let t = r.lanes[0].pairs_out[0].clone();
    r.lanes[0].pairs_out = (0..n)
        .map(|i| {
            let mut p = t.clone();
            p.begin = 2 * i as u64;
            p.end = 2 * i as u64 + 1;
            place(i, &mut p);
            p
        })
        .collect();
    r.lanes.truncate(1);
    Arc::new(v)
}

#[test]
fn c1_10_retained_bar_cap() {
    let n = (1usize << 20) + 1;
    let v = synthetic(n, &|i, p| p.place_b = Place::Placed(1000 + i as u64, 2000 + i as u64));
    let (srcs, infos) = sources(&v, &TimelineBudgets::default());
    let info = &infos[&1];
    assert_eq!(info.not_retained, 1);
    assert!(info.header.contains("; 1 zone not retained"), "{}", info.header);
    match &srcs[0].state {
        FtSourceState::Bars(b) => assert_eq!(b.len(), 1 << 20),
        _ => panic!(),
    }
}

#[test]
fn c1_10_full_view_of_a_million_bars_stops_at_the_inspection_cap() {
    let n = 1_000_000usize;
    let v = synthetic(n, &|i, p| {
        p.place_b = Place::Placed(1000 + i as u64, 1001 + i as u64);
        p.place_e = Place::Placed(1002 + i as u64, 1003 + i as u64);
    });
    let (srcs, _) = sources(&v, &TimelineBudgets::default());
    let t = build_track_axis(&srcs[0], 0, None, ViewBudgets::default());
    assert!(t.refusal.is_none());
    let view = View { t0: 0.0, dt: 1_100_000.0 };
    let l = layout(&t, &view, 1000.0, QueryCaps::default(), true);
    assert_eq!(l.inspected, QueryCaps::default().inspect_cap);
    assert_eq!(l.omitted, Some(None));
    assert!(omission_text(None).contains("count unknown"));
    // the view build budget refuses the lane's drawing when it cannot pay
    let small = build_track_axis(&srcs[0], 0, None, ViewBudgets { build_ops: 1000 });
    assert_eq!(small.refusal.as_deref(), Some("SPU lane 1: drawing REFUSED (view build budget)"));
    assert!(small.bars.is_empty());
}

#[test]
fn unplaced_zones_are_counted_in_the_status_line_never_listed() {
    let v = synthetic(300, &|i, p| {
        p.place_b = Place::Unplaced("order coverage unknown".into());
        p.ticks = Some(i as u128);
    });
    let (srcs, infos) = sources(&v, &TimelineBudgets::default());
    match &srcs[0].state {
        FtSourceState::Bars(b) => assert!(b.is_empty(), "UNPLACED zones are not drawn"),
        _ => panic!(),
    }
    let info = &infos[&1];
    assert_eq!((info.unplaced, info.placed, info.total), (300, 0, 300));
    assert!(info.header.contains("| placed 0/300 pairs | UNPLACED 300 | median width "), "{}", info.header);
    assert!(!info.header.contains('\n'), "one line");
    assert_eq!(status_line("TICKS[profile]", 3, 4, 1, Some(20)), "TICKS[profile] | placed 3/4 pairs | UNPLACED 1 | median width 20 tb");
    assert_eq!(status_line("EMPTY", 0, 0, 0, None), "EMPTY | placed 0/0 pairs | UNPLACED 0 | median width none");
}

#[test]
fn placement_geometry_and_wording() {
    // a placed zone with U(BEGIN) < L(END) has a core; LOWER-ONLY END and UPPER-ONLY BEGIN run to the edges
    let v = synthetic(3, &|i, p| match i {
        0 => {
            p.place_b = Place::Placed(100, 200);
            p.place_e = Place::Placed(300, 400);
        }
        1 => {
            p.place_b = Place::UpperOnly(500);
            p.place_e = Place::Placed(600, 700);
        }
        _ => {
            p.place_b = Place::Placed(800, 900);
            p.place_e = Place::LowerOnly(950);
        }
    });
    let (srcs, infos) = sources(&v, &TimelineBudgets::default());
    let t = build_track_axis(&srcs[0], 0, None, ViewBudgets::default());
    let info = &infos[&1];
    let l = layout(&t, &View { t0: 0.0, dt: 1000.0 }, 1000.0, QueryCaps::default(), true);
    let by_pair = |k: usize| {
        let bi = info.pair_of_bar.iter().position(|&p| p == k).unwrap();
        *l.draws.iter().find(|d| d.kind == ps3dk_prof_viewer::timeline_foreign::FDrawKind::Bar(bi)).unwrap()
    };
    assert_eq!(by_pair(0).core, Some((200.0, 300.0)));
    assert!(by_pair(1).arrow_left && !by_pair(1).arrow_right);
    // LOWER-ONLY END: an open arrow on the right; U(BEGIN) 900 < L(END) 950 still gives a core
    assert!(by_pair(2).arrow_right && !by_pair(2).arrow_left);
    assert_eq!(by_pair(2).core, Some((900.0, 950.0)));
    // UPPER-ONLY BEGIN: U(BEGIN) 500 < L(END) 600 gives a core too
    assert_eq!(by_pair(1).core, Some((500.0, 600.0)));
    // the bars are in the timeline order (unbounded begin first)
    assert_eq!(info.pair_of_bar, vec![1, 0, 2]);
    // wording: the label is ticks; placement "placed between L and U (causal bounds)"; no banned word
    let lines = details(info, "spu.work", 1, Some(79_800_000));
    assert!(lines.iter().any(|x| x == "BEGIN placed between 100 and 200 (causal bounds)"), "{lines:?}");
    let banned = ["duration", "time", "elapsed", "execution", "cpu", "wall", "measured"];
    let mut all = lines.clone();
    all.push(info.header.clone());
    all.push(CAPTION.to_string());
    for x in &all {
        let low = x.to_lowercase();
        for w in banned {
            assert!(!low.split(|c: char| !c.is_ascii_alphanumeric()).any(|t| t == w), "{w:?} in {x}");
        }
    }
    assert!(lines.iter().any(|x| x.ends_with("ticks (SPU decrementer ticks)") || x.contains("ticks (TICKS-CONDITIONAL)")), "{lines:?}");
}

#[test]
fn tooltip_conversion_is_for_the_hovered_pair_only_and_cached() {
    use ps3dk_prof::profile::{FieldRule, TableEntry};
    use ps3dk_prof::spu_ticks::{Iso, SpuEntry, SPU_TICKS_PREMISES};
    let table = vec![SpuEntry {
        entry: TableEntry {
            profile_id: "SPU-TEST".into(),
            kinds: 2,
            premise_bits: SPU_TICKS_PREMISES,
            digests: vec![DIGEST],
            fields: vec![
                (1, FieldRule::Any("t".into())),
                (2, FieldRule::Any("t".into())),
                (3, FieldRule::RequiredMatch(vec![FREQ.to_le_bytes().to_vec()])),
                (4, FieldRule::Any("t".into())),
                (5, FieldRule::RequiredMatch(vec![vec![0], vec![1]])),
                (6, FieldRule::RequiredMatch(vec![vec![0]])),
                (7, FieldRule::RequiredMatch(vec![ext_cfg_bytes()])),
            ],
            evidence: vec!["t".into()],
        },
        rmax: 2,
        iso: Some(Iso { lo: (1, 1), hi: (1, 1), note: String::new() }),
    }];
    let b = f_base().bytes();
    let mut d = doc(&b);
    d.foreign_tl.spu = SpuSlot::with_options(Options { table, ..Options::default() });
    let a = d.analysis().unwrap();
    let mut st = TimelineState::new(a);
    st.sync_foreign(&d.foreign_tl, a);
    let fv = st.foreign.clone().unwrap();
    let (ti, t) = fv.tracks.iter().enumerate().find(|(_, t)| t.spu.is_some()).unwrap();
    let info = t.spu.as_ref().unwrap();
    let mut c = ConvCache::default();
    assert_eq!(conversion_for(&mut c, ti, info, 0, false), None);
    assert_eq!(c.calls, 0);
    for _ in 0..50 {
        let s = conversion_for(&mut c, ti, info, 0, true).unwrap();
        assert!(s.starts_with("at the isolation slope [1, 1] tb/tick"), "{s}");
        assert!(s.ends_with("this figure is not CPU time, execution time or wall time"), "{s}");
    }
    assert_eq!(c.calls, 1, "one conversion for one hovered pair");
    conversion_for(&mut c, ti, info, 1, true).unwrap();
    assert_eq!(c.calls, 2);
    assert_eq!(d.foreign_tl.spu.builds.load(Ordering::Relaxed), 1);
}

#[test]
fn old_files_keep_the_clock_mapping_rows() {
    // a file without order records (flags 3): the SPU ticks analysis is never built and the rows are the old ones
    let mut c = f_base();
    c.flags = 3;
    c.ext.retain(|ch| ch.ty != T_ORDER);
    let b = c.bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let tl = timeline(a);
    let v = d.foreign_tl.view(a, &tl, false);
    assert_eq!(d.foreign_tl.spu.builds.load(Ordering::Relaxed), 0);
    assert!(v.tracks.iter().all(|t| t.spu.is_none()));
    assert!(!v.caption.contains(CAPTION));
    let lane = v.tracks.iter().find(|t| t.label == "SPU lane 1").unwrap();
    assert!(lane.bars.is_empty() && lane.coverage.iter().any(|x| x.contains("ambiguous")));
    let _ = LaneState::Empty;
}

// ---- the drawn Timeline (a headless egui context): bars on the lane rows, no per-pair text in the paint list -------

/// Every text the frame painted (the paint list's text shapes), in paint order.
fn painted_texts(out: &eframe::egui::FullOutput) -> Vec<(eframe::egui::Pos2, eframe::egui::Vec2, String)> {
    use eframe::egui::Shape;
    fn walk(s: &Shape, v: &mut Vec<(eframe::egui::Pos2, eframe::egui::Vec2, String)>) {
        match s {
            Shape::Vec(xs) => xs.iter().for_each(|x| walk(x, v)),
            Shape::Text(t) => v.push((t.pos, t.galley.size(), t.galley.job.text.clone())),
            _ => {}
        }
    }
    let mut v = Vec::new();
    for c in &out.shapes {
        walk(&c.shape, &mut v);
    }
    v
}

struct Page {
    ctx: eframe::egui::Context,
    time: f64,
}

impl Page {
    fn new() -> Page {
        let ctx = eframe::egui::Context::default();
        ps3dk_ui::install(&ctx);
        ctx.style_mut(|s| s.interaction.tooltip_delay = 0.0);
        Page { ctx, time: 0.0 }
    }
    fn frame(&mut self, d: &Document, st: &mut TimelineState, events: Vec<eframe::egui::Event>) -> eframe::egui::FullOutput {
        use eframe::egui::{Pos2, RawInput, Rect, Vec2};
        self.time += 1.0 / 60.0;
        let input = RawInput {
            screen_rect: Some(Rect::from_min_size(Pos2::ZERO, Vec2::new(1600.0, 3000.0))),
            time: Some(self.time),
            events,
            ..Default::default()
        };
        let a = d.analysis().unwrap();
        let mut sel = None;
        self.ctx.run(input, |ctx| ps3dk_prof_viewer::timeline_ui::draw(ctx, &d.foreign_tl, a, st, &mut sel))
    }
}

/// Every horizontal line segment the frame painted (left end first).
fn horizontal_segments(out: &eframe::egui::FullOutput) -> Vec<(eframe::egui::Pos2, eframe::egui::Pos2)> {
    use eframe::egui::Shape;
    fn walk(s: &Shape, v: &mut Vec<(eframe::egui::Pos2, eframe::egui::Pos2)>) {
        match s {
            Shape::Vec(xs) => xs.iter().for_each(|x| walk(x, v)),
            Shape::LineSegment { points: [a, b], .. } if a.y == b.y && a.x != b.x => {
                v.push(if a.x < b.x { (*a, *b) } else { (*b, *a) })
            }
            _ => {}
        }
    }
    let mut v = Vec::new();
    for c in &out.shapes {
        walk(&c.shape, &mut v);
    }
    v
}

/// Per-pair text of the old lane table (any of these in the paint list is a per-pair row).
fn per_pair(t: &str) -> bool {
    t.contains(" seq ") || t.contains("BEGIN UNPLACED") || t.contains("END UNPLACED") || t.contains("BEGIN placed")
}

fn spu_status_texts(texts: &[(eframe::egui::Pos2, eframe::egui::Vec2, String)]) -> Vec<String> {
    texts.iter().filter(|t| t.2.contains("| placed ") && t.2.contains("pairs | UNPLACED")).map(|t| t.2.clone()).collect()
}

#[test]
fn drawn_timeline_without_order_final_paints_one_status_line_per_lane_and_no_pair_text() {
    // a capture that is still arriving, analysed with the final rules: every zone UNPLACED (order coverage unknown)
    let b = f_base_with(false).torn_after(usize::MAX);
    dump("viewer-no-order-final", &b);
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let mut st = TimelineState::new(a);
    st.axis_all = true;
    let mut pg = Page::new();
    pg.frame(&d, &mut st, vec![]);
    let out = pg.frame(&d, &mut st, vec![]);
    let texts = painted_texts(&out);
    assert!(texts.len() > 10, "the page painted its texts");
    for t in &texts {
        assert!(!per_pair(&t.2), "per-pair text in the paint list: {}", t.2);
    }
    let status = spu_status_texts(&texts);
    assert_eq!(status.len(), 2, "one status line per SPU lane: {status:?}");
    for s in &status {
        assert!(s.starts_with("TICKS-UNAVAILABLE (order coverage unknown (no ORDER_FINAL)) | placed 0/16 pairs | UNPLACED 16"), "{s}");
        assert!(!s.contains('\n'), "{s}");
    }
    let fv = st.foreign.clone().unwrap();
    assert!(fv.tracks.iter().filter(|t| t.spu.is_some()).all(|t| t.bars.is_empty()));
    assert_eq!(d.foreign_tl.spu.builds.load(Ordering::Relaxed), 1, "built once for both frames");
}

#[test]
fn drawn_timeline_of_a_provisional_snapshot_draws_bars_and_says_provisional() {
    let b = f_base_with(false).torn_after(usize::MAX);
    let mut d = doc(&b);
    d.foreign_tl.spu.options.provisional = true; // what the live client sets on a snapshot that is still arriving
    let a = d.analysis().unwrap();
    let mut st = TimelineState::new(a);
    st.axis_all = true;
    let mut pg = Page::new();
    pg.frame(&d, &mut st, vec![]);
    let out = pg.frame(&d, &mut st, vec![]);
    let texts = painted_texts(&out);
    for t in &texts {
        assert!(!per_pair(&t.2), "per-pair text in the paint list: {}", t.2);
    }
    let status = spu_status_texts(&texts);
    assert_eq!(status.len(), 2, "{status:?}");
    for s in &status {
        assert!(s.starts_with("PROVISIONAL (no ORDER_FINAL yet) | placed 16/16 pairs | UNPLACED 0 | median width "), "{s}");
    }
    let fv = st.foreign.clone().unwrap();
    for t in fv.tracks.iter().filter(|t| t.spu.is_some()) {
        assert_eq!(t.bars.len(), 16, "{}", t.header);
        let info = t.spu.as_ref().unwrap();
        assert!(info.provisional);
        let lines = details(info, "spu.work", 0, Some(FREQ));
        assert!(lines.iter().any(|l| l == PROVISIONAL_NOTE), "{lines:?}");
        // no tick count in a provisional lane: the tooltip says so in SPU decrementer ticks wording
        assert!(lines[2].starts_with("no tick count (") && lines[2].ends_with("(SPU decrementer ticks)"), "{}", lines[2]);
    }
}

#[test]
fn drawn_bars_carry_ticks_in_the_tooltip_and_a_click_opens_the_details_panel() {
    use eframe::egui::{Event, Modifiers, PointerButton, Pos2};
    use ps3dk_prof_viewer::timeline_ui::ROW_H;
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let mut st = TimelineState::new(a);
    st.axis_all = true;
    let mut pg = Page::new();
    pg.frame(&d, &mut st, vec![]);
    let out = pg.frame(&d, &mut st, vec![]);
    let texts = painted_texts(&out);
    for t in &texts {
        assert!(!per_pair(&t.2), "per-pair text in the paint list: {}", t.2);
    }
    // a drawn bar of SPU lane 1: a horizontal whisker segment in the lane's bar area (right of its label column)
    let lane = texts.iter().find(|t| t.2 == "SPU lane 1").expect("lane label").0;
    let whisker = horizontal_segments(&out)
        .into_iter()
        .find(|(a, b)| a.x > lane.x + 180.0 && a.y > lane.y && a.y < lane.y + ROW_H + 8.0 && b.x - a.x > 6.0)
        .expect("a drawn SPU bar on the lane row");
    let at = Pos2::new((whisker.0.x + whisker.1.x) / 2.0, whisker.0.y);
    // hover: the tooltip carries the tick count in SPU decrementer ticks, and the pair text
    pg.frame(&d, &mut st, vec![Event::PointerMoved(at)]);
    let out = pg.frame(&d, &mut st, vec![Event::PointerMoved(at)]);
    let tip = painted_texts(&out);
    let is_ticks = |t: &str| {
        t.strip_suffix(" ticks (TICKS-CONDITIONAL) (SPU decrementer ticks)").is_some_and(|n| !n.is_empty() && n.chars().all(|c| c.is_ascii_digit()))
    };
    let want = tip.iter().find(|t| is_ticks(&t.2)).map(|t| t.2.clone()).expect("a tooltip line with the tick count");
    assert!(tip.iter().any(|t| t.2.starts_with("BEGIN placed between ")), "the tooltip carries the pair's placement");
    assert!(tip.iter().any(|t| t.2.starts_with("SPU lane 1, epoch ")), "the tooltip names the pair");
    // click: the details panel opens under the lane with the same pair text
    let press = |pressed| Event::PointerButton { pos: at, button: PointerButton::Primary, pressed, modifiers: Modifiers::NONE };
    pg.frame(&d, &mut st, vec![press(true)]);
    pg.frame(&d, &mut st, vec![press(false)]);
    let pick = st.spu_pick.clone().expect("a click on a bar picks it");
    assert!(pick.lines.iter().any(|l| *l == want), "{:?}", pick.lines);
    let out = pg.frame(&d, &mut st, vec![Event::PointerMoved(Pos2::new(1.0, 1.0))]);
    let shown = painted_texts(&out);
    assert!(shown.iter().any(|t| t.2.ends_with(": zone details")), "the details panel is drawn");
    assert!(shown.iter().any(|t| t.2 == want), "the panel shows the tick count");
    // a newer snapshot drops the pick (its pair belongs to the old rows)
    st.refresh(a);
    assert!(st.spu_pick.is_none());
}
