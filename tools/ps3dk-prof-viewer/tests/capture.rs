//! Frame snapshots: exact membership, coverage, bounded work, layout, the ledger and bookmarks (A1 r2: every item of
//! docs/local/prof-viewer/frame-capture-a1-r2-checklist.md has a negative control here or in tests/capture_ui.rs).

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use std::path::{Path, PathBuf};
use std::sync::Arc;

use common::*;
use ps3dk_prof::clockmap::{End, Projection, Rat};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::foreign_timeline::{
    ForeignTimeline, FtBar, FtCounts, FtEpoch, FtEpochRow, FtLimit, FtSource, FtSourceState,
};
use ps3dk_prof_viewer::capture::*;
use ps3dk_prof_viewer::capture_ctl::{Captures, Home};
use ps3dk_prof_viewer::model::Document;

// ---- fixtures ------------------------------------------------------------------------------------------------------

/// Owner lane 1 (tb ascending): FRAMEs 0..3 at base+1000, 4000, 9000, 12000 (frame 1 = [4000, 9000)).  Part A: FRAME
/// 0, an untokened span 3900..4200 (claimed by frame 0, overlapping frame 1), FRAME 1, a span tokened to frame 1
/// (4300..4400), a span begun at 5000 whose END (9500) arrives only in part B, FRAME 2.  Part B: that END and FRAME 3.
/// Lane 2 (no frames, arrives last): 3000..4000 (touches tb), 4500..4600 (overlap), an open span from 8000 with a
/// child 9000..9100 (touches next).
struct Fixture {
    full: Vec<u8>,
    a: Vec<u8>,
    ab: Vec<u8>,
}

fn fixture_at(base: u64) -> Fixture {
    let t = |v: u64| base + v;
    let part_a = [
        vec![frame(0, t(1_000), 0)],
        zone(0, 0, 0, t(3_900), 1, 0),
        vec![frame(0, t(4_000), 1)],
        vec![end(0, 0, 0, t(4_200), 1)],
        zone_tok(0, 0, 0, t(4_300), 2, 0, 1, 1),
        vec![end(0, 0, 0, t(4_400), 2)],
        zone(0, 0, 0, t(5_000), 3, 0),
        vec![frame(0, t(9_000), 2)],
    ]
    .concat();
    let part_b = vec![end(0, 0, 0, t(9_500), 3), frame(0, t(12_000), 3)];
    let n_a = seqd(0, part_a.clone()).len();
    let rec1 = seqd(0, [part_a, part_b].concat());
    let rec2 = seqd(
        0,
        [
            zone(0, 0, 0, t(3_000), 11, 0),
            vec![end(0, 0, 0, t(4_000), 11)],
            zone(0, 0, 0, t(4_500), 10, 0),
            vec![end(0, 0, 0, t(4_600), 10)],
            zone(0, 0, 0, t(8_000), 13, 0),
            zone(0, 0, 1, t(9_000), 12, 13),
            vec![end(0, 0, 1, t(9_100), 12)],
        ]
        .concat(),
    );
    let s = scenario(vec![lane(1, rec1.clone()), lane(2, rec2)]);
    let mut f = s.file();
    let at = f.find(T_RECORDS);
    f.chunks.remove(at);
    f.chunks.insert(at, chunk(T_RECORDS, records_payload(1, &rec1[..n_a])));
    f.chunks.insert(at + 1, chunk(T_RECORDS, records_payload(1, &rec1[n_a..])));
    let full = f.bytes();
    let mut g = f.clone();
    g.chunks.truncate(at + 1);
    let a = g.bytes();
    let mut g = f.clone();
    g.chunks.truncate(at + 2);
    let ab = g.bytes();
    assert!(full.starts_with(&ab) && ab.starts_with(&a));
    Fixture { full, a, ab }
}

fn fixture() -> Fixture {
    fixture_at(0)
}

fn doc(bytes: &[u8]) -> Document {
    Document::from_bytes(PathBuf::from("snap.prof"), bytes)
}

const P: Premise = Premise { w32: true, show_extrapolated: true };

fn fin(v: i128, open: bool) -> End {
    End::At { value: Rat::int(v), open }
}

fn proj(lo: End, hi: End) -> Projection {
    Projection { lo, hi, interpolated: true }
}

fn bar(b: (End, End), e: (End, End), inclusive: u64) -> FtBar {
    FtBar { epoch: 2, image: None, desc: 0, depth: 0, begin: proj(b.0, b.1), end: proj(e.0, e.1), inclusive, exclusive: Some(inclusive), extrapolated: false }
}

fn pt(v: i128) -> (End, End) {
    (fin(v, false), fin(v, false))
}

fn rows(states: Vec<FtEpoch>) -> Vec<FtEpochRow> {
    states.into_iter().enumerate().map(|(i, state)| FtEpochRow { epoch: i as u32 + 1, state, pairs: Some(10) }).collect()
}

fn placed() -> FtEpoch {
    FtEpoch::Placed { bars: 1, too_deep: 0 }
}

fn clean_counts() -> FtCounts {
    FtCounts { open: Some(0), loss_records: Some(0), ..FtCounts::default() }
}

fn source(id: u32, epochs: Vec<FtEpochRow>, counts: FtCounts, bars: Vec<FtBar>) -> FtSource {
    FtSource { source: SourceKey::Lane(id), epochs, counts, state: FtSourceState::Bars(bars) }
}

fn ft_of(sources: Vec<FtSource>) -> (ForeignTimeline, Vec<SourceKey>) {
    let declared = sources.iter().map(|s| s.source).collect();
    (ForeignTimeline { sources, ..ForeignTimeline::default() }, declared)
}

fn origin() -> Origin {
    Origin::Frozen { taken: "t".into() }
}

fn plan_caps(d: &Document, ft: &ForeignTimeline, declared: &[SourceKey], frame: u64, caps: Caps) -> Result<Plan, String> {
    let src = Source { a: d.analysis().unwrap(), ix: d.frames.as_ref().unwrap(), len: d.size as u64 };
    plan(&src, ft, declared, 1, frame, P, &origin(), caps, &mut meter(caps))
}

fn snap_ft(d: &Document, ft: &ForeignTimeline, declared: &[SourceKey], frame: u64, bytes: &[u8]) -> FrameSnapshot {
    let p = plan_caps(d, ft, declared, frame, Caps::default()).unwrap();
    build(p, 1, Arc::new(SnapshotBytes::new("s", bytes.to_vec(), true)), origin())
}

fn snap(d: &Document, sources: Vec<FtSource>, frame: u64, bytes: &[u8]) -> FrameSnapshot {
    let (ft, declared) = ft_of(sources);
    snap_ft(d, &ft, &declared, frame, bytes)
}

fn lane_of(s: &FrameSnapshot, id: LaneId) -> &CLane {
    s.lanes.iter().find(|l| l.id == id).unwrap()
}

// ---- membership ----------------------------------------------------------------------------------------------------

#[test]
fn ppu_classes_are_exact_and_half_open() {
    let k = (1, 1);
    assert_eq!(ppu_class(Some(k), 0, PpuEnd::Measured(1), k, 4000, 9000), Class::Claimed);
    assert_eq!(ppu_class(Some((1, 0)), 3900, PpuEnd::Measured(4200), k, 4000, 9000), Class::Other);
    assert_eq!(ppu_class(Some((7, 1)), 4500, PpuEnd::Measured(4600), k, 4000, 9000), Class::Other, "an unrelated owner's claim");
    assert_eq!(ppu_class(None, 4500, PpuEnd::Measured(4600), k, 4000, 9000), Class::Possible);
    assert_eq!(ppu_class(None, 3000, PpuEnd::Measured(4000), k, 4000, 9000), Class::Outside, "e == tb touches");
    assert_eq!(ppu_class(None, 3000, PpuEnd::Measured(4001), k, 4000, 9000), Class::Possible);
    assert_eq!(ppu_class(None, 9000, PpuEnd::Measured(9100), k, 4000, 9000), Class::Outside, "b == next touches");
    assert_eq!(ppu_class(None, 4000, PpuEnd::Measured(4000), k, 4000, 9000), Class::Possible, "a point at tb is in");
    assert_eq!(ppu_class(None, 9000, PpuEnd::Measured(9000), k, 4000, 9000), Class::Outside, "a point at next is out");
    assert_eq!(ppu_class(None, 8999, PpuEnd::Unavailable, k, 4000, 9000), Class::Possible);
    assert_eq!(ppu_class(None, 1, PpuEnd::Censored, k, 4000, 9000), Class::Possible);
    assert_eq!(ppu_class(None, 9000, PpuEnd::Censored, k, 4000, 9000), Class::Outside);
}

#[test]
fn foreign_bounds_use_earliest_begin_and_latest_end_exactly() {
    let (tb, next) = (exact(4000), exact(9000));
    let b = |b, e, n| bar(b, e, n);
    assert!(inside(&fin(4000, false), &fin(5000, false), &tb, &next));
    assert!(inside(&fin(4000, true), &fin(9000, true), &tb, &next), "open hi at next stays inside");
    assert!(!inside(&fin(4000, false), &fin(9000, false), &tb, &next), "closed hi at next is outside");
    assert!(!inside(&End::NegInf, &fin(5000, false), &tb, &next));
    assert!(!inside(&fin(5000, false), &End::PosInf, &tb, &next));
    assert!(foreign_possible(&b((fin(3000, false), fin(8000, false)), (fin(3500, false), fin(9500, false)), 5), &tb, &next));
    assert!(!foreign_possible(&b((End::NegInf, fin(3000, false)), (fin(3500, false), fin(4000, false)), 5), &tb, &next));
    assert!(foreign_possible(&b((End::NegInf, fin(3000, false)), (fin(3500, false), End::PosInf), 5), &tb, &next));
    assert!(!foreign_possible(&b(pt(9000), pt(9500), 5), &tb, &next));
    assert!(foreign_possible(&b((fin(8999, true), fin(9000, false)), pt(9500), 5), &tb, &next));
    assert!(foreign_possible(&b((fin(3000, false), fin(4000, false)), pt(0), 0), &tb, &next));
    assert!(!foreign_possible(&b((fin(3000, false), fin(4000, true)), pt(0), 0), &tb, &next));
    assert!(!foreign_possible(&b((End::PosInf, End::PosInf), pt(0), 0), &tb, &next));
    let _ = ratio(1, 2);
}

#[test]
fn every_span_is_in_exactly_one_class() {
    let fx = fixture();
    let d = doc(&fx.full);
    let s = snap(&d, vec![], 1, &fx.full);
    let l1 = lane_of(&s, LaneId::Ppu(1));
    assert_eq!((l1.evidence.claimed, l1.evidence.possible, l1.evidence.other), (2, 0, 1), "{:?}", l1.evidence);
    assert_eq!(l1.status, Status::Observed);
    let l2 = lane_of(&s, LaneId::Ppu(2));
    assert_eq!((l2.evidence.claimed, l2.evidence.possible, l2.evidence.other), (0, 2, 0), "{:?}", l2.evidence);
    assert_eq!((l2.evidence.open_members, l2.status), (1, Status::Partial));
    let total: u64 = s.lanes.iter().map(|l| l.evidence.claimed + l.evidence.possible).sum();
    assert_eq!(total, s.bars.len() as u64);
    for l in s.lanes.iter() {
        assert!(s.bars[l.start as usize..l.end as usize].iter().all(|b| s.lanes[b.lane as usize].id == l.id), "bars grouped by lane");
    }
}

#[test]
fn claimed_extrapolated_and_possible_only_foreign_pairs() {
    let fx = fixture();
    let d = doc(&fx.full);
    let mut ex = bar(pt(5000), pt(6000), 10);
    ex.extrapolated = true;
    let s = snap(
        &d,
        vec![
            source(0, rows(vec![placed()]), clean_counts(), vec![bar(pt(5000), pt(6000), 10)]),
            source(1, rows(vec![placed()]), clean_counts(), vec![ex]),
            source(2, rows(vec![placed()]), clean_counts(), vec![bar((fin(3000, false), fin(8000, false)), (fin(3500, false), fin(9500, false)), 5)]),
            source(3, rows(vec![placed()]), clean_counts(), vec![bar((fin(1500, false), fin(1600, false)), (fin(4500, false), fin(4600, false)), 5)]),
            source(4, rows(vec![placed()]), clean_counts(), vec![bar(pt(9000), pt(9100), 5)]),
        ],
        1,
        &fx.full,
    );
    let e = |i| lane_of(&s, LaneId::Spu(i)).clone();
    assert_eq!((e(0).evidence.claimed, e(0).status), (1, Status::Observed));
    assert_eq!((e(1).evidence.claimed, e(1).evidence.extrapolated, e(1).status), (1, 1, Status::Partial));
    assert_eq!((e(2).evidence.claimed, e(2).evidence.possible, e(2).status), (0, 1, Status::Observed), "wide uncertainty: possible only");
    assert_eq!((e(3).evidence.claimed, e(3).evidence.possible, e(3).evidence.other), (0, 0, 1));
    assert_eq!((e(4).evidence.claimed + e(4).evidence.possible, e(4).status), (0, Status::NoData));
    assert_eq!(s.sources_declared, 5);
}

// ---- coverage ------------------------------------------------------------------------------------------------------

#[test]
fn an_unmappable_epoch_is_unknown_and_keeps_the_usable_ones() {
    let fx = fixture();
    let d = doc(&fx.full);
    let mut states = vec![FtEpoch::NoObservations];
    states.extend((0..7).map(|_| placed()));
    let s = snap(
        &d,
        vec![
            source(0, rows(states), clean_counts(), vec![bar(pt(5000), pt(6000), 10)]),
            source(1, rows(vec![placed()]), FtCounts { loss_records: Some(3), ..clean_counts() }, vec![]),
            source(2, rows(vec![placed()]), FtCounts { open: Some(2), ..clean_counts() }, vec![]),
            source(3, rows(vec![FtEpoch::AmbiguousLift, FtEpoch::AmbiguousLift]), clean_counts(), vec![]),
            FtSource { source: SourceKey::Gpu(9), epochs: vec![], counts: clean_counts(), state: FtSourceState::Refused(FtLimit::CaptureOps) },
            source(5, vec![FtEpochRow { epoch: 1, state: FtEpoch::AmbiguousLift, pairs: Some(0) }, FtEpochRow { epoch: 2, state: placed(), pairs: Some(1) }], clean_counts(), vec![]),
        ],
        1,
        &fx.full,
    );
    let l0 = lane_of(&s, LaneId::Spu(0));
    assert_eq!((l0.evidence.placed_epochs, l0.evidence.unknown.get(&EpochWhy::NoObservations)), (7, Some(&1)));
    assert_eq!((l0.evidence.claimed, l0.status), (1, Status::Partial), "usable epochs kept, never NO DATA");
    assert_eq!(lane_of(&s, LaneId::Spu(1)).status, Status::Partial, "loss-only");
    assert_eq!(lane_of(&s, LaneId::Spu(2)).status, Status::Partial, "open-only");
    assert_eq!(lane_of(&s, LaneId::Spu(3)).status, Status::Unmappable);
    let r = lane_of(&s, LaneId::Rsx(9));
    assert_eq!((r.evidence.refused, r.status), (Some(SourceRefusal::Budget), Status::Unmappable));
    assert_eq!(lane_of(&s, LaneId::Spu(5)).status, Status::NoData);
}

#[test]
fn a_retention_refusal_keeps_every_declared_source_as_unmappable() {
    let fx = fixture();
    let d = doc(&fx.full);
    let ft = ForeignTimeline { retention_refused: true, ..ForeignTimeline::default() };
    let declared = vec![SourceKey::Lane(1), SourceKey::Lane(2), SourceKey::Gpu(7)];
    let s = snap_ft(&d, &ft, &declared, 1, &fx.full);
    assert_eq!(s.sources_declared, 3);
    for k in &declared {
        let l = lane_of(&s, LaneId::of(*k));
        assert_eq!((l.status, l.evidence.refused), (Status::Unmappable, Some(SourceRefusal::Retention)), "{k:?}");
        assert!(l.evidence.items().contains(&("RETENTION".to_string(), Some(1))));
    }
    // a declared source the placement never listed (no epochs): present, NO DATA
    let (ft, _) = ft_of(vec![]);
    let s = snap_ft(&d, &ft, &[SourceKey::Lane(4)], 1, &fx.full);
    assert_eq!(lane_of(&s, LaneId::Spu(4)).status, Status::NoData);
}

// ---- bounded work --------------------------------------------------------------------------------------------------

#[test]
fn a_placement_error_refuses_the_snapshot_atomically() {
    let fx = fixture();
    let d = doc(&fx.full);
    // BEGIN in frame 0 (outside the selected frame 1): inside() is false, so place() runs for it, as the LAST work.
    let (ft, declared) = ft_of(vec![source(0, rows(vec![placed()]), clean_counts(), vec![bar((fin(1500, false), fin(1600, false)), (fin(4500, false), fin(4600, false)), 5)])]);
    let need = (1..1_000_000u64)
        .find(|&ops| plan_caps(&d, &ft, &declared, 1, Caps { ops, ..Caps::default() }).is_ok())
        .unwrap();
    let ok = plan_caps(&d, &ft, &declared, 1, Caps { ops: need, ..Caps::default() }).unwrap();
    assert!(ok.bound > 0);
    // every smaller budget refuses (never a partial result), and some budget runs out INSIDE place(): that refusal
    // is propagated, not taken as Possible/Outside
    let mut inside_place = 0;
    for ops in 0..need {
        let e = plan_caps(&d, &ft, &declared, 1, Caps { ops, ..Caps::default() }).err().unwrap_or_else(|| panic!("{ops}: a result below the threshold"));
        if e.starts_with("placing a SPU lane 0 pair was refused") {
            inside_place += 1;
        }
    }
    assert!(inside_place > 0, "a budget exhausted inside place() refuses the snapshot");
}

#[test]
fn the_bar_and_transient_caps_refuse() {
    let fx = fixture();
    let d = doc(&fx.full);
    let (ft, declared) = ft_of(vec![source(0, rows(vec![placed()]), clean_counts(), vec![bar(pt(5000), pt(6000), 10), bar(pt(5100), pt(6100), 10)])]);
    // frame 1 members: 2 PPU on lane 1, 2 on lane 2, 2 SPU
    assert!(plan_caps(&d, &ft, &declared, 1, Caps { bars: 6, ..Caps::default() }).is_ok());
    assert_eq!(plan_caps(&d, &ft, &declared, 1, Caps { bars: 5, ..Caps::default() }).err().unwrap(), "the frame has more than 5 member bars: not captured");
    let used = plan_caps(&d, &ft, &declared, 1, Caps::default()).unwrap().transient;
    assert!(plan_caps(&d, &ft, &declared, 1, Caps { transient: used, ..Caps::default() }).is_ok());
    let e = plan_caps(&d, &ft, &declared, 1, Caps { transient: used - 1, ..Caps::default() }).err().unwrap();
    assert!(e.contains("would allocate more than"), "{e}");
}

#[test]
fn the_default_frame_search_is_metered() {
    let fx = fixture();
    let d = doc(&fx.full);
    let o = d.frames.as_ref().unwrap().owner(1).unwrap();
    let latest = vec![Some(exact(20_000))];
    let mut m = meter(Caps { ops: 1_000, ..Caps::default() });
    assert_eq!(choose_frame(o, &latest, &mut m).unwrap(), Some(2));
    assert!(m.m.used() >= o.slots.len() as u64);
    let mut m = meter(Caps { ops: 1, ..Caps::default() });
    assert!(choose_frame(o, &latest, &mut m).is_err());
    let (ft, declared) = ft_of(vec![source(0, rows(vec![placed()]), clean_counts(), vec![bar(pt(5000), pt(6000), 10); 50])]);
    let mut m = meter(Caps { ops: 10, ..Caps::default() });
    assert!(latest_begins(&ft, &declared, &mut m).is_err(), "scanning 50 bars is charged");
}

// ---- endpoints and layout ------------------------------------------------------------------------------------------

#[test]
fn coordinates_past_2_pow_53_stay_exact() {
    let base = 1u64 << 53;
    let fx = fixture_at(base);
    let d = doc(&fx.full);
    let s = snap(&d, vec![], 1, &fx.full);
    let l1 = lane_of(&s, LaneId::Ppu(1));
    let b = s.bars[l1.start as usize..l1.end as usize].iter().find(|b| b.b_lo == Pt::exact(300)).expect("4300 - 4000");
    assert_eq!(b.e_hi, Pt::exact(400));
    // a naive (begin as f64 - tb as f64) at this magnitude is not exact for odd offsets
    let naive = (base + 4_301) as f64 - (base + 4_000) as f64;
    assert_ne!(naive, 301.0, "the control: f64 first loses the tick");
    assert_eq!(Pt::exact(i128::from(base + 4_301) - i128::from(base + 4_000)).f(), Some(301.0));
}

fn one_bar_snapshot(b: FtBar) -> FrameSnapshot {
    let fx = fixture();
    let d = doc(&fx.full);
    snap(&d, vec![source(0, rows(vec![placed()]), clean_counts(), vec![b])], 1, &fx.full)
}

fn spu_draw(s: &FrameSnapshot, view: View, width: f64) -> Vec<SDraw> {
    let i = s.lanes.iter().position(|l| l.id == LaneId::Spu(0)).unwrap();
    layout(s, i..i + 1, view, width, PaintCaps::default()).draws
}

#[test]
fn open_bounds_keep_their_openness_and_fractional_bounds_round_outward() {
    // BEGIN in [4000 + 1/3, 9000) open at next_tb: claimed (inside), text says open; the lower bound rounds down
    let mut b = bar((End::At { value: ratio(3 * 4000 + 1, 3), open: false }, fin(9000, true)), (fin(9500, false), fin(9600, false)), 10);
    b.exclusive = Some(10);
    let s = one_bar_snapshot(b);
    let c = s.bars[lane_of(&s, LaneId::Spu(0)).start as usize];
    assert_eq!(c.class, Class::Claimed);
    assert_eq!(c.b_lo, Pt::At { ticks: 0, open: false, exact: false }, "1/3 tick rounds outward to 0");
    assert_eq!(c.b_hi, Pt::At { ticks: 5000, open: true, exact: true });
    let text = ps3dk_prof_viewer::capture_ui::details(&s, &c).join(" | ");
    assert!(text.contains("rounded outward") && text.contains("open (approached, not attained)"), "{text}");
}

#[test]
fn layout_rounds_cores_inward_whiskers_outward_and_marks_ticks() {
    // core [5000, 5000.5): under a pixel at 1 px per tick after inward rounding -> no core
    let s = one_bar_snapshot(bar((fin(4990, false), fin(5000, false)), (End::At { value: ratio(10_001, 2), open: false }, fin(5010, false)), 5));
    let d = spu_draw(&s, View { t0: 0.0, dt: 9000.0 }, 9000.0);
    assert_eq!(d.len(), 1);
    assert!(d[0].core.is_none(), "a sub-pixel guaranteed core is not invented");
    assert_eq!((d[0].w0, d[0].w1), (990.0, 1010.0), "whisker rounded outward");
    // the same bar zoomed out: whisker under a pixel -> a tick
    let d = spu_draw(&s, View { t0: 0.0, dt: 9000.0 }, 100.0);
    assert!(d[0].tick && d[0].core.is_none());
    // an exact point (inclusive 0): a tick at any zoom where it is under a pixel
    let s = one_bar_snapshot(bar(pt(5000), pt(5000), 0));
    assert!(spu_draw(&s, View { t0: 0.0, dt: 9000.0 }, 900.0)[0].tick);
    // an unbounded end: an arrow to the edge
    let s = one_bar_snapshot(bar(pt(5000), (fin(6000, false), End::PosInf), 10));
    let d = spu_draw(&s, View { t0: 0.0, dt: 9000.0 }, 900.0);
    assert!(d[0].arrow_right && d[0].w1 == 900.0);
    // claimed + extrapolated: drawn with the conditional style flag
    let mut x = bar(pt(5000), pt(6000), 10);
    x.extrapolated = true;
    let s = one_bar_snapshot(x);
    let d = spu_draw(&s, View { t0: 0.0, dt: 9000.0 }, 900.0);
    assert!(d[0].extrapolated && d[0].class == Class::Claimed && d[0].core.is_some());
}

#[test]
fn paint_work_is_capped_with_visible_omissions() {
    let fx = fixture();
    let d = doc(&fx.full);
    let bars: Vec<FtBar> = (0..100).map(|i| bar(pt(5000 + i), pt(5100 + i), 10)).collect();
    let s = snap(&d, vec![source(0, rows(vec![placed()]), clean_counts(), bars)], 1, &fx.full);
    let i = s.lanes.iter().position(|l| l.id == LaneId::Spu(0)).unwrap();
    let v = View { t0: 0.0, dt: 9000.0 };
    let l = layout(&s, i..i + 1, v, 900.0, PaintCaps { inspect: 1000, paint: 10 });
    assert_eq!((l.draws.len(), l.omitted), (10, Some(Some(90))));
    let l = layout(&s, i..i + 1, v, 900.0, PaintCaps { inspect: 20, paint: 1000 });
    assert_eq!((l.inspected, l.omitted), (20, Some(None)));
    // row culling: lanes outside the range are not inspected
    let l = layout(&s, 0..1, v, 900.0, PaintCaps::default());
    assert_eq!(l.inspected as u32, s.lanes[0].end - s.lanes[0].start);
}

// ---- snapshots of a growing capture ----------------------------------------------------------------------------------

#[test]
fn a_snapshot_never_changes_and_later_data_only_shows_in_a_new_one() {
    let fx = fixture();
    let (da, dab) = (doc(&fx.a), doc(&fx.ab));
    let sa = snap(&da, vec![], 1, &fx.a);
    let sab = snap(&dab, vec![], 1, &fx.ab);
    let a1 = lane_of(&sa, LaneId::Ppu(1));
    assert_eq!((a1.evidence.claimed, a1.evidence.open_members, a1.status), (2, 1, Status::Partial));
    let a2 = lane_of(&sa, LaneId::Ppu(2));
    assert_eq!((a2.evidence.claimed + a2.evidence.possible, a2.status), (0, Status::NoData));
    let b1 = lane_of(&sab, LaneId::Ppu(1));
    assert_eq!((b1.evidence.claimed, b1.evidence.open_members, b1.status), (2, 0, Status::Observed));
}

// ---- the ledger ----------------------------------------------------------------------------------------------------

fn take(c: &mut Captures, d: &Document, bytes: &[u8], premise: Premise) -> Result<Arc<FrameSnapshot>, String> {
    let b = bytes.to_vec();
    c.capture(d, Some(1), Some(1), premise, "t0", &mut |n| (n as usize == b.len()).then(|| b.clone()))
}

#[test]
fn the_charge_is_reserved_before_retaining_and_bounds_what_is_held() {
    let fx = fixture();
    let d = doc(&fx.full);
    let mut c = Captures::new("live:x", Home::Live);
    let base = c.ledger.used;
    assert!(base > 0, "the list's metadata is charged");
    let s = take(&mut c, &d, &fx.full, P).unwrap();
    assert!(s.retained_bytes() <= s.charge, "{} > {}", s.retained_bytes(), s.charge);
    assert_eq!(c.ledger.used, base + s.charge + s.snap.charge());
    let s2 = c.reresolve(&s, Premise { w32: false, ..P }).unwrap();
    assert!(Arc::ptr_eq(&s.snap, &s2.snap));
    assert_eq!(c.ledger.used, base + s.charge + s2.charge + s.snap.charge());
}

#[test]
fn recorded_statuses_are_charged_as_supplied() {
    let fx = fixture();
    let d = doc(&fx.full);
    let (ft, declared) = ft_of(vec![]);
    let src = Source { a: d.analysis().unwrap(), ix: d.frames.as_ref().unwrap(), len: d.size as u64 };
    let recorded: Box<[(LaneId, Status)]> = (0..256).map(|i| (LaneId::Spu(i), Status::Observed)).collect();
    let o = Origin::Reresolved { file: "f.prof".into(), mark: 1, snap_bytes: 1, recorded };
    let p = plan(&src, &ft, &declared, 1, 1, P, &o, Caps::default(), &mut meter(Caps::default())).unwrap();
    let small = plan_caps(&d, &ft, &declared, 1, Caps::default()).unwrap().bound;
    assert!(p.bound >= small + 256 * std::mem::size_of::<(LaneId, Status)>() as u64, "{} vs {small}", p.bound);
    let s = build(p, 1, Arc::new(SnapshotBytes::new("s", fx.full.clone(), true)), o);
    assert!(s.retained_bytes() <= s.charge);
}

#[test]
fn a_refusal_retains_nothing_and_copies_nothing() {
    let fx = fixture();
    let d = doc(&fx.full);
    let mut c = Captures::with_budget("live:x", Home::Live, 4096);
    let base = c.ledger.used;
    let mut fetched = false;
    let r = c.capture(&d, Some(1), Some(1), P, "t0", &mut |_| {
        fetched = true;
        None
    });
    let e = r.unwrap_err();
    assert!(e.starts_with("Not captured: snapshots would hold"), "{e}");
    assert!(!fetched);
    assert_eq!((c.ledger.used, c.list.len(), c.ledger.retained_snapshots()), (base, 0, 0));
}

#[test]
fn deleting_a_displayed_snapshot_refunds_only_when_the_display_lets_go() {
    let fx = fixture();
    let d = doc(&fx.full);
    let mut c = Captures::new("live:x", Home::Live);
    let base = c.ledger.used;
    let s = take(&mut c, &d, &fx.full, P).unwrap();
    let s2 = c.reresolve(&s, Premise { w32: false, ..P }).unwrap();
    let shown = s.clone();
    let (c1, c2, b, id) = (s.charge, s2.charge, s.snap.charge(), s.id);
    drop(s);
    c.delete(id);
    assert_eq!(c.ledger.used, base + c1 + c2 + b, "displayed: nothing refunded");
    drop(shown);
    c.sweep();
    assert_eq!(c.ledger.used, base + c2 + b);
    drop(s2);
    c.clear();
    c.sweep();
    assert_eq!((c.ledger.used, c.ledger.retained_snapshots()), (base, 0));
}

#[test]
fn the_count_cap_frees_on_delete_and_ids_are_never_reused() {
    let fx = fixture();
    let d = doc(&fx.full);
    let mut c = Captures::new("live:x", Home::Live);
    let mut ids = Vec::new();
    for _ in 0..MAX_SNAPSHOTS {
        ids.push(take(&mut c, &d, &fx.full, P).unwrap().id);
    }
    assert!(take(&mut c, &d, &fx.full, P).unwrap_err().contains("is the limit"));
    c.delete(ids[4]);
    let n = take(&mut c, &d, &fx.full, P).unwrap().id;
    assert!(!ids.contains(&n), "a fresh in-memory id");
}

#[test]
fn file_captures_are_bound_to_the_analysed_bytes() {
    let fx = fixture();
    let d = doc(&fx.full);
    let mut c = Captures::new("file:x", Home::Live);
    c.bind_source(&fx.full);
    let mut same_size = fx.full.clone();
    let n = same_size.len();
    same_size[n - 1] ^= 0xff;
    let e = c.capture(&d, Some(1), Some(1), P, "t", &mut |_| Some(same_size.clone())).unwrap_err();
    assert_eq!(e, "Not captured: the file changed since it was opened.");
    assert_eq!((c.list.len(), c.ledger.retained_snapshots()), (0, 0));
    assert!(c.capture(&d, Some(1), Some(1), P, "t", &mut |_| Some(fx.full.clone())).is_ok());
}

/// The hardware capture of hw3-run1: run with `--ignored` and PS3DK_HW3_CAPTURE set.
#[test]
#[ignore]
fn hw3_capture_snapshot_fits_the_budget() {
    let p = std::env::var("PS3DK_HW3_CAPTURE").expect("PS3DK_HW3_CAPTURE names the hw3 .prof");
    let bytes = std::fs::read(&p).unwrap();
    let d = doc(&bytes);
    let owner = d.frames.as_ref().unwrap().default_owner().unwrap();
    let mut c = Captures::new("file:hw3", Home::Live);
    let b = bytes.clone();
    let t = std::time::Instant::now();
    let s = c.capture(&d, Some(owner), None, P, "hw3", &mut |n| (n as usize == b.len()).then(|| b.clone())).unwrap();
    let ms = t.elapsed().as_millis();
    let s2 = c.reresolve(&s, Premise { w32: false, ..P }).unwrap();
    println!(
        "HW3 snapshot frame {} [{}, {}): bars {}, lanes {}, declared {}, extract charge {} B (held {} B), bytes charge {} B, ledger {} of {} B, freeze {} ms",
        s.frame, s.tb, s.next_tb, s.bars.len(), s.lanes.len(), s.sources_declared, s.charge, s.retained_bytes(), s.snap.charge(), c.ledger.used, c.ledger.budget, ms
    );
    for l in s.lanes.iter() {
        println!("  W32 {} {}: {}", l.id.code(), l.status.word(), l.evidence.lines().join("; "));
    }
    for l in s2.lanes.iter().filter(|l| l.id.foreign()) {
        println!("  R64 {} {}", l.id.code(), l.status.word());
    }
    assert!(s.retained_bytes() <= s.charge && s2.retained_bytes() <= s2.charge);
    assert!(c.ledger.used <= c.ledger.budget);
    assert_eq!(s.sources_declared, 7);
    let spu: Vec<_> = s.lanes.iter().filter(|l| matches!(l.id, LaneId::Spu(_))).collect();
    assert_eq!(spu.len(), 6);
    assert!(spu.iter().all(|l| l.evidence.claimed + l.evidence.possible > 0 && l.status == Status::Partial));
    assert!(s.lanes.iter().any(|l| matches!(l.id, LaneId::Rsx(_)) && l.evidence.claimed > 0));
    assert!(s2.lanes.iter().filter(|l| matches!(l.id, LaneId::Spu(_))).all(|l| l.status == Status::Unmappable));
}

// ---- bookmarks -----------------------------------------------------------------------------------------------------

fn dir(name: &str) -> PathBuf {
    let d = PathBuf::from(env!("CARGO_TARGET_TMPDIR")).join(format!("capture-{name}-{}", std::process::id()));
    std::fs::create_dir_all(&d).unwrap();
    let _ = std::fs::remove_file(d.join("cap.frames.txt"));
    d
}

/// A live session with snapshots of A and A+B, published as the full file.
fn live_two(d: &Path) -> (PathBuf, Captures, Fixture) {
    let fx = fixture();
    let saved = d.join("cap.prof");
    std::fs::write(&saved, &fx.full).unwrap();
    let mut c = Captures::new("live:x", Home::Live);
    c.capture(&doc(&fx.a), Some(1), Some(1), P, "t1", &mut |_| Some(fx.a.clone())).unwrap();
    c.capture(&doc(&fx.ab), Some(1), Some(1), P, "t2", &mut |_| Some(fx.ab.clone())).unwrap();
    (saved, c, fx)
}

#[test]
fn reopening_re_resolves_against_the_whole_saved_file() {
    let d = dir("two");
    let (saved, mut c, fx) = live_two(&d);
    let note = c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    assert!(note.contains("2 snapshot bookmark(s)"), "{note}");
    let mut r = Captures::new("file:cap", Home::File(saved.clone()));
    r.bind_source(&fx.full);
    r.load_marks(&saved, &fx.full);
    assert!(r.refused_lines.is_empty(), "{:?}", r.refused_lines);
    assert_eq!(r.marks.len(), 2);
    let ra = r.open_mark(0, &saved).unwrap();
    let rb = r.open_mark(1, &saved).unwrap();
    assert!(!Arc::ptr_eq(&ra, &rb));
    // both re-resolved against the full file: the late END and lane 2 are in
    for s in [&ra, &rb] {
        assert_eq!(s.snap.bytes.len(), fx.full.len());
        assert_eq!(lane_of(s, LaneId::Ppu(1)).status, Status::Observed);
        assert_eq!(lane_of(s, LaneId::Ppu(2)).evidence.possible, 2);
    }
    // the recorded statuses and prefixes stay distinct
    let rec = |s: &FrameSnapshot| match &s.origin {
        Origin::Reresolved { snap_bytes, recorded, .. } => (*snap_bytes, recorded.iter().find(|(id, _)| *id == LaneId::Ppu(1)).unwrap().1),
        _ => panic!(),
    };
    assert_eq!(rec(&ra), (fx.a.len() as u64, Status::Partial));
    assert_eq!(rec(&rb), (fx.ab.len() as u64, Status::Observed));
    assert!(Arc::ptr_eq(&ra.snap, &rb.snap), "one copy of the file");
    // opening again shows the same snapshot (no new id, no growth)
    let again = r.open_mark(0, &saved).unwrap();
    assert!(Arc::ptr_eq(&again, &ra));
    assert_eq!(r.list.len(), 2);
}

#[test]
fn all_64_bookmarks_open_and_reopening_never_exhausts() {
    let d = dir("64");
    let fx = fixture();
    let saved = d.join("cap.prof");
    std::fs::write(&saved, &fx.full).unwrap();
    let mut c = Captures::new("live:x", Home::Live);
    for _ in 0..MAX_SNAPSHOTS {
        c.capture(&doc(&fx.full), Some(1), Some(1), P, "t", &mut |_| Some(fx.full.clone())).unwrap();
    }
    c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    let mut r = Captures::new("file:cap", Home::File(saved.clone()));
    r.bind_source(&fx.full);
    r.load_marks(&saved, &fx.full);
    assert_eq!(r.marks.len(), 64);
    assert_eq!(r.marks.iter().map(|m| m.record.id).max(), Some(64));
    for round in 0..3 {
        for i in 0..64 {
            r.open_mark(i, &saved).unwrap_or_else(|e| panic!("round {round} mark {i}: {e}"));
        }
    }
    assert_eq!(r.list.len(), 64);
}

#[test]
fn opening_rechecks_the_whole_header() {
    let d = dir("hdr");
    let (saved, mut c, fx) = live_two(&d);
    c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    let mut r = Captures::new("file:cap", Home::File(saved.clone()));
    r.bind_source(&fx.full);
    r.load_marks(&saved, &fx.full);
    // bytes appended after the recorded prefixes: the prefixes still verify, the header does not
    let mut grown = fx.full.clone();
    grown.push(0);
    std::fs::write(&saved, &grown).unwrap();
    let e = r.open_mark(0, &saved).unwrap_err();
    assert!(e.contains("over the") || e.contains("the file changed"), "{e}");
    // a same-size change
    let mut same = fx.full.clone();
    let n = same.len();
    same[n - 1] ^= 1;
    std::fs::write(&saved, &same).unwrap();
    assert!(r.open_mark(0, &saved).unwrap_err().contains("the file changed"));
    assert!(r.list.is_empty());
}

#[test]
fn bookmark_header_and_lines_are_validated() {
    let fx = fixture();
    let d = dir("val");
    let saved = d.join("v.prof");
    std::fs::write(&saved, &fx.full).unwrap();
    let mut c = Captures::new("live:x", Home::Live);
    let s = c.capture(&doc(&fx.ab), Some(1), Some(1), P, "t", &mut |_| Some(fx.ab.clone())).unwrap();
    let h = header_for("v.prof", &fx.full).unwrap();
    let good = record_line(&bookmark_of(&s, 1).unwrap());
    let mut b2 = bookmark_of(&s, 2).unwrap();
    b2.snap_sha256 = [0u8; 32];
    let not_prefix = record_line(&b2);
    let text = [
        header_line(&h),
        good.clone(),
        not_prefix,
        good.clone(),
        good.replace(" owner=1 ", " owner=01 "),
        good.replace(" frame=1 ", " frame=18446744073709551616 "),
        good.replace("CLAIMED:", "BOGUS:"),
        "garbage".into(),
    ]
    .join("\n")
        + "\n";
    std::fs::write(d.join("v.frames.txt"), &text).unwrap();
    let mut r = Captures::new("file:v", Home::File(saved.clone()));
    r.bind_source(&fx.full);
    r.load_marks(&saved, &fx.full);
    assert_eq!(r.marks.len(), 2);
    assert!(r.marks[1].refused.as_deref().unwrap().contains("not a prefix"));
    assert_eq!(r.refused_lines.len(), 5, "{:?}", r.refused_lines);
    assert!(r.frozen_file);
    assert!(r.write_file_bookmarks(&saved, &mut RealIo::default()).unwrap_err().contains("left unchanged"));
    let mut other = fx.full.clone();
    other.push(0);
    let mut r2 = Captures::new("file:v", Home::File(saved.clone()));
    r2.load_marks(&saved, &other);
    assert!(r2.marks.is_empty() && r2.refused_lines.iter().any(|(_, e)| e.contains("bytes")));
}

#[test]
fn bookmark_reads_are_bounded_before_allocation() {
    let fx = fixture();
    let d = dir("big");
    let saved = d.join("b.prof");
    std::fs::write(&saved, &fx.full).unwrap();
    std::fs::write(d.join("b.frames.txt"), vec![b'x'; MAX_FILE + 1]).unwrap();
    let mut r = Captures::new("file:b", Home::File(saved.clone()));
    r.load_marks(&saved, &fx.full);
    assert!(r.marks.is_empty() && r.frozen_file);
    assert!(r.refused_lines[0].1.contains("over the"), "{:?}", r.refused_lines);
    assert!(read_bounded(&saved, fx.full.len() as u64 - 1).unwrap_err().contains("over the"));
}

#[test]
fn bookmark_parse_limits() {
    assert_eq!(parse_u64("0"), Some(0));
    assert_eq!(parse_u64("00"), None);
    assert_eq!(parse_u64("+1"), None);
    assert_eq!(parse_u64("18446744073709551615"), Some(u64::MAX));
    assert_eq!(parse_u64("18446744073709551616"), None);
    let h = Header { file: "a.prof".into(), bytes: 1, sha256: [1; 32] };
    let mut many = header_line(&h);
    for _ in 0..=MAX_SNAPSHOTS {
        many.push_str("\nx");
    }
    assert!(parse(many.as_bytes()).refused[0].1.contains("more than"));
    let long = format!("{}\n{}\n", header_line(&h), "c".repeat(MAX_LINE + 1));
    assert_eq!(parse(long.as_bytes()).refused[0], (2, "line too long".to_string()));
    assert!(parse(b"PS3DK_FRAME_CAPTURE v2 file=a.prof bytes=1 sha256=00\n").refused[0].1.contains("version"));
    assert!(header_for("has space.prof", b"x").is_err());
}

#[test]
fn more_lanes_than_a_record_carries_are_refused_not_truncated() {
    let fx = fixture();
    let d = doc(&fx.full);
    let (ft, _) = ft_of(vec![]);
    let declared: Vec<SourceKey> = (0..MAX_STATUS_ITEMS as u32).map(SourceKey::Lane).collect();
    let s = snap_ft(&d, &ft, &declared, 1, &fx.full);
    assert!(s.lanes.len() > MAX_STATUS_ITEMS);
    let e = bookmark_of(&s, 1).unwrap_err();
    assert!(e.contains("a bookmark records at most 256"), "{e}");
}

/// Fails one named production step; delegates the rest to the real operations.
struct Faulty {
    fail: &'static str,
    real: RealIo,
    removed: usize,
}

impl BookmarkIo for Faulty {
    fn open(&mut self, tmp: &Path) -> std::io::Result<()> {
        let r = self.real.open(tmp);
        if self.fail == "open" {
            return Err(std::io::Error::other("injected open"));
        }
        r
    }
    fn write(&mut self, b: &[u8]) -> std::io::Result<()> {
        if self.fail == "write" {
            return Err(std::io::Error::other("injected write"));
        }
        self.real.write(b)
    }
    fn sync(&mut self) -> std::io::Result<()> {
        if self.fail == "sync" {
            return Err(std::io::Error::other("injected sync"));
        }
        self.real.sync()
    }
    fn close(&mut self) -> std::io::Result<()> {
        let r = self.real.close();
        if self.fail == "close" {
            return Err(std::io::Error::other("injected close"));
        }
        r
    }
    fn rename(&mut self, a: &Path, b: &Path) -> std::io::Result<()> {
        if self.fail == "rename" {
            return Err(std::io::Error::other("injected rename"));
        }
        self.real.rename(a, b)
    }
    fn remove(&mut self, p: &Path) {
        self.removed += 1;
        self.real.remove(p)
    }
}

#[test]
fn every_publication_step_failure_leaves_the_previous_file_and_no_phantom_bookmark() {
    for fail in ["open", "write", "sync", "close", "rename"] {
        let d = dir(&format!("pub-{fail}"));
        let (saved, mut c, fx) = live_two(&d);
        c.delete(2); // keep one snapshot for the first, successful publication
        c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
        let bp = d.join("cap.frames.txt");
        let before = std::fs::read(&bp).unwrap();
        let marks_before = c.marks.len();
        // a new snapshot of the published file (not yet written), then a failing rewrite
        c.home = Home::Live;
        c.capture(&doc(&fx.full), Some(1), Some(0), P, "t3", &mut |_| Some(fx.full.clone())).unwrap();
        c.home = Home::File(saved.clone());
        let mut io = Faulty { fail, real: RealIo::default(), removed: 0 };
        let e = c.write_file_bookmarks(&saved, &mut io).unwrap_err();
        assert!(e.starts_with("Bookmarks not saved") && e.contains(fail), "{fail}: {e}");
        assert_eq!(std::fs::read(&bp).unwrap(), before, "{fail}: the previous file is intact");
        assert!(!d.join("cap.frames.txt.tmp").exists(), "{fail}");
        assert_eq!(io.removed, 1);
        assert_eq!(c.marks.len(), marks_before, "{fail}: no phantom bookmark in memory");
        // a later successful write includes the snapshot
        c.write_file_bookmarks(&saved, &mut RealIo::default()).unwrap();
        assert_eq!(c.marks.len(), marks_before + 1, "{fail}");
    }
}

#[test]
fn a_snapshot_not_a_prefix_of_the_saved_file_is_not_bookmarked() {
    let fx = fixture();
    let d = dir("np");
    let saved = d.join("cap.prof");
    std::fs::write(&saved, &fx.a).unwrap();
    let mut c = Captures::new("live:x", Home::Live);
    c.capture(&doc(&fx.ab), Some(1), Some(1), P, "t", &mut |_| Some(fx.ab.clone())).unwrap();
    let note = c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    assert!(note.contains("1 snapshot(s) are not a prefix"), "{note}");
    assert!(!d.join("cap.frames.txt").exists() || parse(&std::fs::read(d.join("cap.frames.txt")).unwrap()).records.is_empty());
}

#[test]
fn snapshots_after_the_outcome_are_bookmarked_at_once_or_said_unsaved() {
    let d = dir("after");
    let (saved, mut c, fx) = live_two(&d);
    c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    assert_eq!(c.home, Home::File(saved.clone()));
    let n = c.marks.len();
    c.capture(&doc(&fx.full), Some(1), Some(0), P, "late", &mut |_| Some(fx.full.clone())).unwrap();
    assert_eq!(c.marks.len(), n + 1, "persisted immediately");
    assert_eq!(parse(&std::fs::read(d.join("cap.frames.txt")).unwrap()).records.len(), n + 1);
    // no file saved
    let mut u = Captures::new("live:y", Home::Unsaved);
    u.capture(&doc(&fx.full), Some(1), Some(1), P, "t", &mut |_| Some(fx.full.clone())).unwrap();
    assert!(u.message.as_ref().unwrap().1.contains("not bookmarked: the live session saved no file"));
}

// ---- r3: metadata commit, own-bytes binding, prefix exclusion, charge-before-copy, overflow endpoints -------------

/// A live session with snapshot A, published as the full file (first bookmark write), then one more snapshot of the
/// full file written as a second bookmark write.
fn two_writes(d: &Path, budget: u64) -> (Captures, Result<(), String>, Vec<u8>, usize) {
    let fx = fixture();
    let saved = d.join("cap.prof");
    std::fs::write(&saved, &fx.full).unwrap();
    let _ = std::fs::remove_file(d.join("cap.frames.txt"));
    let mut c = Captures::with_budget("live:x", Home::Live, budget);
    c.capture(&doc(&fx.a), Some(1), Some(1), P, "t1", &mut |_| Some(fx.a.clone())).unwrap();
    c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    let first = std::fs::read(d.join("cap.frames.txt")).unwrap();
    let marks = c.marks.len();
    c.home = Home::Live; // capture without the automatic write, then write explicitly
    c.capture(&doc(&fx.full), Some(1), Some(0), P, "t2", &mut |_| Some(fx.full.clone())).unwrap();
    c.home = Home::File(saved.clone());
    let r = c.write_file_bookmarks(&saved, &mut RealIo::default());
    (c, r, first, marks)
}

#[test]
fn a_metadata_refusal_preserves_disk_marks_ids_and_ledger_exact_and_one_short() {
    let d = dir("meta-big");
    let (big, r, _, _) = two_writes(&d, RETAIN_BUDGET);
    r.unwrap();
    let need = big.ledger.used;
    drop(big);
    let d = dir("meta-exact");
    let (c, r, _, _) = two_writes(&d, need);
    r.unwrap();
    assert_eq!((c.marks.len(), c.ledger.used), (2, need));
    let d = dir("meta-short");
    let (mut c, r, first, marks) = two_writes(&d, need - 1);
    let e = r.unwrap_err();
    assert!(e.starts_with("Bookmarks not saved: Not captured: snapshots would hold"), "{e}");
    assert_eq!(std::fs::read(d.join("cap.frames.txt")).unwrap(), first, "disk unchanged");
    assert_eq!(c.marks.len(), marks, "marks unchanged");
    assert_eq!(c.marks.iter().map(|m| m.record.id).collect::<Vec<_>>(), vec![1]);
    assert_eq!(c.ledger.pool().held(), c.ledger.used, "nothing left reserved");
    // freeing room lets the pending snapshot be written under the NEXT id; record 1 stays on disk
    let first_id = c.list[0].id;
    c.delete(first_id);
    c.sweep();
    let saved = d.join("cap.prof");
    c.write_file_bookmarks(&saved, &mut RealIo::default()).unwrap();
    let p = parse(&std::fs::read(d.join("cap.frames.txt")).unwrap());
    assert_eq!(p.records.iter().map(|r| r.id).collect::<Vec<_>>(), vec![1, 2]);
}

#[test]
fn sessions_on_one_pool_share_the_aggregate_budget() {
    let fx = fixture();
    let pool = Pool::new(RETAIN_BUDGET);
    let mut a = Captures::with_pool("live:a", Home::Live, pool.clone());
    let _s = take(&mut a, &doc(&fx.full), &fx.full, P).unwrap();
    let held = pool.held();
    assert_eq!(held, a.ledger.used);
    let b = Captures::with_pool("live:b", Home::Live, pool.clone());
    let meta = b.ledger.used;
    assert_eq!(pool.held(), held + meta, "both sessions counted");
    drop(b);
    assert_eq!(pool.held(), held, "a dropped session returns what it held");
    // a pool holding exactly one session's use plus a second session's metadata refuses the second snapshot
    let tight = Pool::new(held + meta);
    let mut x = Captures::with_pool("live:x", Home::Live, tight.clone());
    let mut y = Captures::with_pool("live:y", Home::Live, tight.clone());
    take(&mut x, &doc(&fx.full), &fx.full, P).unwrap();
    assert!(take(&mut y, &doc(&fx.full), &fx.full, P).unwrap_err().starts_with("Not captured: snapshots would hold"));
}

#[test]
fn a_prefix_snapshot_re_resolves_from_its_own_bytes_after_the_session_ended() {
    let d = dir("own");
    let fx = fixture();
    let saved = d.join("cap.prof");
    std::fs::write(&saved, &fx.full).unwrap();
    let mut c = Captures::new("live:x", Home::Live);
    let a = c.capture(&doc(&fx.a), Some(1), Some(1), P, "t1", &mut |_| Some(fx.a.clone())).unwrap();
    c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    let r = c.reresolve(&a, Premise { w32: false, ..P }).unwrap();
    assert!(Arc::ptr_eq(&r.snap, &a.snap));
    assert_eq!(r.snap.bytes.len(), fx.a.len());
    let e = c.capture(&doc(&fx.a), Some(1), Some(1), P, "t2", &mut |_| Some(fx.a.clone())).unwrap_err();
    assert!(e.contains("not the bytes this session is bound to"), "{e}");
}

#[test]
fn a_snapshot_excluded_as_not_a_prefix_stays_excluded_on_every_later_write() {
    let d = dir("np2");
    let fx = fixture();
    let saved = d.join("cap.prof");
    std::fs::write(&saved, &fx.a).unwrap();
    let mut c = Captures::new("live:x", Home::Live);
    c.capture(&doc(&fx.ab), Some(1), Some(1), P, "t", &mut |_| Some(fx.ab.clone())).unwrap();
    let note = c.write_live_bookmarks(&saved, &mut RealIo::default()).unwrap();
    assert!(note.contains("1 snapshot(s) are not a prefix"), "{note}");
    for k in [0u64, 1] {
        c.capture(&doc(&fx.a), Some(1), Some(k), P, "later", &mut |_| Some(fx.a.clone())).unwrap();
    }
    let p = parse(&std::fs::read(d.join("cap.frames.txt")).unwrap());
    assert_eq!(p.records.len(), 2);
    let ok = verify_prefixes(&fx.a, &p.records.iter().map(|r| (r.snap_bytes, r.snap_sha256)).collect::<Vec<_>>());
    assert!(ok.iter().all(|x| *x), "every written record is a prefix of the saved file");
}

#[test]
fn a_long_label_is_refused_before_it_is_copied() {
    let recs = seqd(
        0,
        [vec![frame(0, 1_000, 0), frame(0, 4_000, 1)], zone(0, 0, 0, 4_100, 1, 0), vec![end(0, 0, 0, 4_200, 1), frame(0, 9_000, 2)]]
            .concat(),
    );
    let mut s = scenario(vec![lane(1, recs)]);
    s.descs[0].name = vec![b'n'; 250];
    let bytes = s.bytes();
    let d = doc(&bytes);
    let (ft, declared) = ft_of(vec![]);
    let need = plan_caps(&d, &ft, &declared, 1, Caps::default()).unwrap().transient;
    assert!(d.analysis().is_some() && need > 250);
    assert!(plan_caps(&d, &ft, &declared, 1, Caps { transient: need, ..Caps::default() }).is_ok());
    let e = plan_caps(&d, &ft, &declared, 1, Caps { transient: need - 1, ..Caps::default() }).err().unwrap();
    assert!(e.contains("would allocate more than"), "{e}");
    let mut w = meter(Caps { transient: 200, ..Caps::default() });
    let src = Source { a: d.analysis().unwrap(), ix: d.frames.as_ref().unwrap(), len: d.size as u64 };
    let e = plan(&src, &ft, &declared, 1, 1, P, &origin(), Caps::default(), &mut w).err().unwrap();
    assert!(e.contains("would allocate more than"), "{e}");
    assert_eq!(w.transient, 0, "refused at the first label's charge, before any copy");
}

#[test]
fn many_declared_sources_are_metered() {
    let keys: Vec<SourceKey> = (0..20_000u32).map(|i| SourceKey::Lane(i % 5_000)).collect();
    let mut w = meter(Caps::default());
    let out = dedup_sources(keys.iter().copied(), keys.len(), &mut w).unwrap();
    assert_eq!(out.len(), 5_000);
    assert_eq!(out[..3], [SourceKey::Lane(0), SourceKey::Lane(1), SourceKey::Lane(2)]);
    let used = w.m.used();
    assert!(dedup_sources(keys.iter().copied(), keys.len(), &mut meter(Caps { ops: used, ..Caps::default() })).is_ok());
    assert!(dedup_sources(keys.iter().copied(), keys.len(), &mut meter(Caps { ops: used - 1, ..Caps::default() })).is_err());
    let fx = fixture();
    let d = doc(&fx.full);
    let (ft, _) = ft_of(vec![]);
    let declared: Vec<SourceKey> = (0..5_000u32).map(SourceKey::Lane).collect();
    assert!(plan_caps(&d, &ft, &declared, 1, Caps { ops: 5_000, ..Caps::default() }).is_err());
    let p = plan_caps(&d, &ft, &declared, 1, Caps::default()).unwrap();
    assert!(p.transient > 5_000 * 32, "names and lanes charged: {}", p.transient);
}

#[test]
fn vector_growth_is_charged() {
    let fx = fixture();
    let d = doc(&fx.full);
    let (ft0, dec0) = ft_of(vec![source(0, rows(vec![placed()]), clean_counts(), vec![])]);
    let (ft1, dec1) = ft_of(vec![source(0, rows(vec![placed()]), clean_counts(), vec![bar(pt(5000), pt(6000), 10)])]);
    let t0 = plan_caps(&d, &ft0, &dec0, 1, Caps::default()).unwrap().transient;
    let t1 = plan_caps(&d, &ft1, &dec1, 1, Caps::default()).unwrap().transient;
    assert!(t1 - t0 >= 4 * pending_bytes(), "growth charged: {} vs {}", t1 - t0, 4 * pending_bytes());
}

fn big(sign: i128) -> Rat {
    use num_bigint::BigInt;
    // a 512-bit rational about 2^128: num 2^500 + 1, den 2^372
    let num: BigInt = (BigInt::from(1) << 500usize) + 1;
    Rat { num: if sign < 0 { -num } else { num }, den: BigInt::from(1) << 372usize }
}

#[test]
fn finite_bounds_past_i128_are_overflow_endpoints_not_finite_positions() {
    for (sign, upper) in [(1, true), (1, false), (-1, true), (-1, false)] {
        let e = End::At { value: big(sign), open: false };
        let p = Pt::of(&e, 0, upper);
        assert_eq!(p, Pt::Beyond { negative: sign < 0, open: false }, "sign {sign} upper {upper}");
        assert_eq!(p.f(), None);
        let t = pt_text(p, None);
        assert!(t.starts_with(if sign < 0 { "finite, but below" } else { "finite, but above" }) && !t.contains("rounded outward"), "{t}");
    }
    let b = bar((fin(5000, false), End::At { value: big(1), open: false }), (fin(6000, false), End::At { value: big(1), open: false }), 10);
    let s = one_bar_snapshot(b);
    let c = s.bars[lane_of(&s, LaneId::Spu(0)).start as usize];
    assert!(matches!(c.b_hi, Pt::Beyond { negative: false, .. }));
    let d = spu_draw(&s, View { t0: 0.0, dt: 9000.0 }, 900.0);
    assert!(d[0].core.is_none() && d[0].arrow_right && d[0].w1 == 900.0);
}
