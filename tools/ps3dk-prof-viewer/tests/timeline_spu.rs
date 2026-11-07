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
        c.finish(l);
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
        assert!(t.header.starts_with("SPU decrementer ticks, placed by causal bounds | zones drawn 16, UNPLACED 0"), "{}", t.header);
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
fn c1_10_lane_table_paints_at_most_256_rows() {
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
    let names = |_d: u32| "spu.work".to_string();
    let (rows, rest) = table_rows(info, &names, 0);
    assert_eq!(rows.len(), 256);
    assert_eq!(not_shown_line(rest).as_deref(), Some("44 not shown"));
    // sorted by ticks, largest first
    assert!(rows[0].contains(": 299 ticks"), "{}", rows[0]);
    assert!(rows[0].contains("BEGIN UNPLACED (order coverage unknown)"), "{}", rows[0]);
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
