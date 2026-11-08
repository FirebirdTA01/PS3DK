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
    // wording: the label is ticks; a final placement "placed between L and U (final bounds: causal, tightened by the
    // RMAX1 assumption)"; no banned word
    let lines = details(info, "spu.work", 1, Some(79_800_000));
    assert!(
        lines.iter().any(|x| x == "BEGIN placed between 100 and 200 (final bounds: causal, tightened by the RMAX1 assumption)"),
        "{lines:?}"
    );
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

// ---- the SPU rows' work bound: one charge admits each lane before any scan, allocation or sort --------------------

fn row_budgets(capture_ops: u64, retained_bars: u64) -> TimelineBudgets {
    TimelineBudgets { capture_ops, retained_bars, ..TimelineBudgets::default() }
}

/// Every admitted lane: its counted steps never exceed the charge that admitted it; a refused lane did no step.
#[track_caller]
fn prep_bounded(infos: &std::collections::BTreeMap<u32, SpuTrackInfo>) {
    for (id, i) in infos {
        assert!(i.prep.steps <= i.prep.charged, "lane {id}: {:?}", i.prep);
        if i.header.contains("rows UNSUPPORTED-RESOURCE") {
            assert_eq!(i.prep, SpuPrep::default(), "lane {id}: a refused lane did nothing");
        }
    }
}

#[test]
fn spu_rows_zero_budget_refuses_every_lane_before_any_scan_and_the_refusal_is_cached() {
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let v = d.foreign_tl.spu.get(a);
    let (srcs, infos) = sources(&v, &row_budgets(0, 1 << 20));
    prep_bounded(&infos);
    assert_eq!(infos.len(), 2);
    for (s, (id, i)) in srcs.iter().zip(&infos) {
        assert!(matches!(&s.state, FtSourceState::Bars(b) if b.is_empty()));
        assert!(i.header.starts_with(&format!("SPU lane {id}: rows UNSUPPORTED-RESOURCE (SPU row budget: 0 of 0")), "{}", i.header);
        assert!(i.header.ends_with("nothing drawn"), "{}", i.header);
        assert!(i.pair_of_bar.is_empty());
    }
    // through the Document: the refusal is on the track, and the rows are built once
    let mut d = doc(&b);
    d.foreign_tl.spu_rows = row_budgets(0, 1 << 20);
    let a = d.analysis().unwrap();
    let mut st = TimelineState::new(a);
    for _ in 0..5 {
        st.sync_foreign(&d.foreign_tl, a);
    }
    let first = st.foreign.clone().unwrap();
    st.sync_foreign(&d.foreign_tl, a);
    assert!(Arc::ptr_eq(&first, st.foreign.as_ref().unwrap()), "the refused rows are cached");
    assert_eq!(d.foreign_tl.builds.load(Ordering::Relaxed), 1);
    assert_eq!(d.foreign_tl.spu.builds.load(Ordering::Relaxed), 1);
    let lanes: Vec<_> = first.tracks.iter().filter(|t| t.spu.is_some()).collect();
    assert_eq!(lanes.len(), 2);
    for t in lanes {
        assert!(t.bars.is_empty());
        let r = t.refusal.as_deref().unwrap_or("");
        assert!(r.contains("rows UNSUPPORTED-RESOURCE (SPU row budget"), "{r}");
        assert!(t.header.contains("rows UNSUPPORTED-RESOURCE"), "{}", t.header);
    }
}

#[test]
fn spu_rows_exact_budget_admits_a_lane_and_one_short_refuses_it_and_every_later_lane() {
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let v = d.foreign_tl.spu.get(a);
    let r = v.result.as_ref().unwrap();
    let cost = |i: usize, retain: u64| row_ops(r.lanes[i].segments.len() as u64, r.lanes[i].pairs_out.len() as u64, retain);
    let (c1, c2) = (cost(0, 1 << 20), cost(1, (1 << 20) - 16));
    let (_, full) = sources(&v, &TimelineBudgets::default());
    prep_bounded(&full);
    assert_eq!((full[&1].prep.charged, full[&2].prep.charged), (c1, c2));
    // the counted steps of a lane are what it did: its segments, its pairs and their pushes, both sorts, its bars
    for (id, i) in &full {
        assert!(i.prep.steps > 1 + r.lanes[(*id - 1) as usize].pairs_out.len() as u64, "lane {id}: {:?}", i.prep);
    }
    let (_, exact) = sources(&v, &row_budgets(c1 + c2, 1 << 20));
    assert_eq!(exact, full, "exact budget: the same rows");
    let (_, short) = sources(&v, &row_budgets(c1 + c2 - 1, 1 << 20));
    prep_bounded(&short);
    assert_eq!(short[&1], full[&1], "lane 1 is paid");
    assert!(short[&2].header.starts_with("SPU lane 2: rows UNSUPPORTED-RESOURCE"), "{}", short[&2].header);
    let (_, first_short) = sources(&v, &row_budgets(c1 - 1, 1 << 20));
    prep_bounded(&first_short);
    assert!(first_short.values().all(|i| i.header.contains("rows UNSUPPORTED-RESOURCE")), "lane 1 and every later lane");
}

#[test]
fn spu_rows_with_retain_zero_skip_the_bar_sort_and_count_every_zone_as_not_retained() {
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let v = d.foreign_tl.spu.get(a);
    let (srcs, infos) = sources(&v, &row_budgets(1 << 28, 0));
    prep_bounded(&infos);
    for (s, i) in srcs.iter().zip(infos.values()) {
        assert!(matches!(&s.state, FtSourceState::Bars(b) if b.is_empty()));
        assert_eq!(i.not_retained, 16);
        assert!(i.header.contains("; 16 zones not retained"), "{}", i.header);
        let u = i.lane_out();
        assert_eq!(i.prep.charged, row_ops(u.segments.len() as u64, 16, 0));
        assert!(i.prep.charged < row_ops(u.segments.len() as u64, 16, 1), "no bar sort is paid for");
    }
}

/// A result of `lanes` lanes, each a copy of lane 1 with `pairs` copies of its first pair (0: no pair, no segment).
fn synthetic_lanes(lanes: u32, pairs: usize) -> Arc<SpuView> {
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let mut v = (*d.foreign_tl.spu.get(a)).clone();
    let r = v.result.as_mut().unwrap();
    let mut t = r.lanes[0].clone();
    let p = t.pairs_out[0].clone();
    t.pairs_out = (0..pairs).map(|_| p.clone()).collect();
    if pairs == 0 {
        t.segments.clear();
    }
    r.lanes = (1..=lanes).map(|id| {
        let mut l = t.clone();
        l.lane_id = id;
        l
    }).collect();
    Arc::new(v)
}

#[test]
fn spu_rows_of_many_empty_lanes_are_charged_a_fixed_amount_each_and_refused_once_unpaid() {
    let v = synthetic_lanes(20_000, 0);
    let per = row_ops(0, 0, 1 << 20);
    assert_eq!(per, ROW_FIXED_OPS);
    let (_, all) = sources(&v, &row_budgets(per * 20_000, 1 << 20));
    prep_bounded(&all);
    assert!(all.values().all(|i| i.prep.charged == per && !i.header.contains("UNSUPPORTED")));
    let (_, half) = sources(&v, &row_budgets(per * 10_000, 1 << 20));
    prep_bounded(&half);
    let refused = half.values().filter(|i| i.header.contains("rows UNSUPPORTED-RESOURCE")).count();
    assert_eq!(refused, 10_000);
    assert!(half[&10_000].prep.charged == per && half[&10_001].header.contains("rows UNSUPPORTED-RESOURCE"));
}

#[test]
fn spu_rows_of_a_lane_with_many_pairs_are_refused_before_its_scan_and_sorts() {
    let n = 1usize << 18;
    let v = synthetic_lanes(1, n);
    let need = row_ops(v.result.as_ref().unwrap().lanes[0].segments.len() as u64, n as u64, 1 << 20);
    assert!(need > 4 * n as u64, "the charge covers the scan and both sorts: {need}");
    let (srcs, short) = sources(&v, &row_budgets(need - 1, 1 << 20));
    prep_bounded(&short);
    assert_eq!(short[&1].prep, SpuPrep::default(), "nothing scanned or sorted");
    assert!(matches!(&srcs[0].state, FtSourceState::Bars(b) if b.is_empty()));
    let (srcs, ok) = sources(&v, &row_budgets(need, 1 << 20));
    prep_bounded(&ok);
    assert_eq!(ok[&1].prep.charged, need);
    assert!(matches!(&srcs[0].state, FtSourceState::Bars(b) if b.len() == n));
}

// ---- the cache key: every option the result depends on -----------------------------------------------------------

#[test]
fn spu_cache_rebuilds_when_the_rmax1_rate_changes() {
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let slot = SpuSlot::default();
    let one = slot.get_with(a, &Options::default());
    assert_eq!(slot.get_with(a, &Options::default()).rmax1, (1, 1));
    assert_eq!(slot.builds.load(Ordering::Relaxed), 1);
    let half = slot.get_with(a, &Options { rmax1: (2, 1), ..Options::default() });
    assert_eq!(slot.builds.load(Ordering::Relaxed), 2, "a changed rate is another key");
    assert_eq!(half.rmax1, (2, 1));
    let places = |v: &SpuView| -> Vec<Place> {
        v.result.as_ref().unwrap().lanes.iter().flat_map(|l| l.pairs_out.iter().map(|p| p.place_b.clone())).collect()
    };
    assert_ne!(places(&one), places(&half), "the placements depend on the rate");
    assert!(Arc::ptr_eq(&half, &slot.get_with(a, &Options { rmax1: (2, 1), ..Options::default() })));
    assert_eq!(slot.builds.load(Ordering::Relaxed), 2);
    assert_ne!(SpuKey::of(&Options::default()), SpuKey::of(&Options { rmax1: (2, 1), ..Options::default() }));
}

#[test]
fn spu_cache_rebuilds_for_a_same_id_entry_with_changed_rules_or_premises() {
    use ps3dk_prof::profile::FieldRule;
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let slot = SpuSlot::default();
    let base = test_table();
    let mut rules = base.clone();
    rules[0].entry.fields[2].1 = FieldRule::RequiredMatch(vec![(FREQ + 1).to_le_bytes().to_vec()]);
    let mut premises = base.clone();
    premises[0].entry.premise_bits ^= 1 << 3;
    for t in [&rules, &premises] {
        assert_eq!((&t[0].entry.profile_id, &t[0].entry.digests), (&base[0].entry.profile_id, &base[0].entry.digests), "same id");
    }
    let held = slot.get_with(a, &Options { table: base.clone(), ..Options::default() });
    assert!(held.result.as_ref().unwrap().classification.holds());
    let changed = slot.get_with(a, &Options { table: rules.clone(), ..Options::default() });
    assert_eq!(slot.builds.load(Ordering::Relaxed), 2, "same id, changed rules: another key");
    assert!(!changed.result.as_ref().unwrap().classification.holds(), "the new rules no longer hold");
    let p = slot.get_with(a, &Options { table: premises.clone(), ..Options::default() });
    assert_eq!(slot.builds.load(Ordering::Relaxed), 3, "same id, changed premises: another key");
    assert!(!p.result.as_ref().unwrap().classification.failures.is_empty());
    let again = slot.get_with(a, &Options { table: premises, ..Options::default() });
    assert!(Arc::ptr_eq(&p, &again));
    assert_eq!(slot.builds.load(Ordering::Relaxed), 3);
}

// ---- wording: provisional causal-only bounds, final causal-plus-RMAX1 bounds -------------------------------------

#[test]
fn final_and_provisional_bounds_are_named_and_the_causal_bounds_stay_inspectable() {
    let banned = ["duration", "time", "elapsed", "execution", "cpu", "wall", "measured", "ran", "running", "descheduled", "descheduling"];
    let words_ok = |x: &str| {
        let low = x.to_lowercase();
        for w in banned {
            assert!(!low.split(|c: char| !c.is_ascii_alphanumeric()).any(|t| t == w), "{w:?} in {x}");
        }
    };
    // FINAL: tightened by RMAX1, the causal-only bounds beside them, the assumption named
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let v = d.foreign_tl.spu.get(a);
    let (_, infos) = sources(&v, &TimelineBudgets::default());
    let info = &infos[&1];
    let p = info.pair(0);
    assert!(p.rmax1_b && p.rmax1_e);
    let lines = details(info, "spu.work", 0, Some(FREQ));
    assert!(lines[3].starts_with("BEGIN placed between ") && lines[3].ends_with("(final bounds: causal, tightened by the RMAX1 assumption)"), "{lines:?}");
    assert!(lines.contains(&format!("BEGIN {}", place_text(&p.causal_b, Basis::CausalOnly))), "{lines:?}");
    assert!(lines.contains(&format!("END {}", place_text(&p.causal_e, Basis::CausalOnly))), "{lines:?}");
    let u = info.lane_out();
    let o = &u.obs[p.b];
    assert_eq!(p.causal_b, Place::Placed(o.l.unwrap(), o.u.unwrap()), "the causal-only bounds are the reader's");
    assert!(lines.iter().any(|l| l.starts_with("RMAX1 assumption: the SPU decrementer advances at most 1 tick(s) per 1 tb tick(s)")
        && l.contains("hold only under it")), "{lines:?}");
    assert!(!lines.iter().any(|l| l.contains("PROVISIONAL")), "{lines:?}");
    lines.iter().for_each(|l| words_ok(l));
    // PROVISIONAL: causal only, no RMAX1 chain, no assumption line
    let pb = f_base_with(false).torn_after(usize::MAX);
    let mut pd = doc(&pb);
    pd.foreign_tl.spu.options.provisional = true;
    let pa = pd.analysis().unwrap();
    let pv = pd.foreign_tl.spu.get(pa);
    let (_, pinfos) = sources(&pv, &TimelineBudgets::default());
    let plines = details(&pinfos[&1], "spu.work", 0, Some(FREQ));
    assert!(plines[3].ends_with("(PROVISIONAL bounds: causal only, no RMAX1 chain yet)"), "{plines:?}");
    assert!(!plines.iter().any(|l| l.starts_with("RMAX1 assumption") || l.contains("tightened")), "{plines:?}");
    assert!(plines.iter().any(|l| l == PROVISIONAL_NOTE));
    plines.iter().for_each(|l| words_ok(l));
    // FINAL, but the chain did not run over the segment: causal only, said so
    let nv = synthetic(1, &|_, p| {
        p.rmax1_b = false;
        p.rmax1_e = false;
    });
    let (_, ninfos) = sources(&nv, &TimelineBudgets::default());
    let nlines = details(&ninfos[&1], "spu.work", 0, Some(FREQ));
    assert!(nlines[3].ends_with("(final bounds: causal only, the RMAX1 chain did not apply)"), "{nlines:?}");
    assert!(!nlines.iter().any(|l| l.starts_with("RMAX1 assumption")), "{nlines:?}");
    // the caption: both kinds named, the solid core conditional on RMAX1, the causal-only bounds in the details
    for want in [
        "FINAL bounds are the causal bounds tightened by the RMAX1 assumption",
        "PROVISIONAL bounds (a capture still arriving) are the causal bounds alone",
        "which for final bounds holds only under the RMAX1 assumption",
        "the details keep each zone's causal-only bounds",
    ] {
        assert!(CAPTION.contains(want), "{want:?} not in {CAPTION}");
    }
    assert!(!CAPTION.contains("causal bounds only (no"), "{CAPTION}");
    words_ok(CAPTION);
    words_ok(PROVISIONAL_NOTE);
}

/// The slot does no work beside the analysis: at a zero capture budget the analysis refuses before any step, and no
/// drawing map is built before or beside it (each pair's depth and each lane's image come with the result).
#[test]
fn spu_slot_at_a_zero_budget_does_no_work_before_or_beside_the_analysis() {
    let b = f_base().bytes();
    let d = doc(&b);
    let a = d.analysis().unwrap();
    let slot = SpuSlot::default();
    let v = slot.get_with(a, &Options { capture_ops: 0, ..Options::default() });
    let r = v.result.as_ref().unwrap();
    assert_eq!((r.work.steps, r.work.own_charged, r.used), (0, 0, 0));
    assert!(r.lanes.is_empty() && r.exhausted_at.is_some());
    let (srcs, infos) = sources(&v, &TimelineBudgets::default());
    assert!(srcs.is_empty() && infos.is_empty());
    assert!(Arc::ptr_eq(&v, &slot.get_with(a, &Options { capture_ops: 0, ..Options::default() })), "the refusal is cached");
    assert_eq!(slot.builds.load(Ordering::Relaxed), 1);
    // with a budget, the drawing facts are the decoded ones
    let full = slot.get_with(a, &Options::default());
    let x = a.capture.ext.as_ref().unwrap();
    for u in &full.result.as_ref().unwrap().lanes {
        assert_eq!(u.image, x.lanes.get(&u.lane_id).map(|l| l.image_index));
        let s = x.sources.iter().find(|s| s.key == ps3dk_prof::ext::SourceKey::Lane(u.lane_id)).unwrap();
        for p in &u.pairs_out {
            let raw = s.raw.pairs.iter().find(|q| q.begin == p.begin).unwrap();
            assert_eq!(p.depth, raw.depth);
        }
    }
}
