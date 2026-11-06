//! Frames page graph modes (crate::graphs): the PPU series, breaks, smoothing windows, the exact histogram, the
//! verdict cells and the per-repaint bounds, on captures from the reader tests' independent writer.  The SPU/RSX
//! lane side of the same modes is tested in tests/foreign.rs beside its handcrafted reader results.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof::{load, Analysis, Metric, Outcome};
use ps3dk_prof_viewer::frames::{summary, Budget, FramesIndex, SlotKind};
use ps3dk_prof_viewer::graphs::*;

const F: u64 = 79_800_000; // the scenario CLOCKS frequency
const B30: u64 = F / 30;

fn analysed(bytes: &[u8]) -> Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

fn index(recs: Vec<Lane>) -> FramesIndex {
    FramesIndex::build(&analysed(&scenario(recs).bytes()))
}

fn frames_at(tbs: &[u64]) -> Vec<Rec> {
    tbs.iter().enumerate().map(|(i, &tb)| frame(0, tb, i as u64)).collect()
}

#[test]
fn the_default_mode_is_bars_and_every_mode_has_a_distinct_label() {
    assert_eq!(GraphMode::default(), GraphMode::Bars);
    assert_eq!(Smoothing::default(), Smoothing::Off);
    let mut l: Vec<&str> = GraphMode::ALL.iter().map(|m| m.label()).collect();
    l.sort();
    l.dedup();
    assert_eq!(l.len(), GraphMode::ALL.len());
    assert!(GraphMode::ALL.iter().filter(|m| !m.per_frame()).eq([GraphMode::Distribution].iter()));
    assert_eq!(Smoothing::ALL.map(|s| s.window()), [None, Some(5), Some(15), Some(30)]);
}

#[test]
fn ppu_points_are_exact_and_break_at_unknown_and_refused() {
    // FRAME 0..3: 1000, B30 + 1000, B30 + 1000 (a measured 0), final; the final frame is UNKNOWN.
    let t0 = 1_000;
    let ix = index(vec![lane(1, seqd(0, frames_at(&[t0, t0 + B30, t0 + B30, t0 + B30 + 7])))]);
    let o = ix.owner(1).unwrap();
    assert_eq!(ppu_point(o, 0), LinePoint::At { y: B30 as f64, band: None });
    assert_eq!(ppu_point(o, 1), LinePoint::At { y: 0.0, band: None }, "a measured 0 is a point at 0");
    assert_eq!(ppu_point(o, 2), LinePoint::At { y: 7.0, band: None });
    assert_eq!(ppu_point(o, 3), LinePoint::Unknown, "the final frame is never a point");
    assert_eq!(ppu_point(o, 99), LinePoint::NoInterval);
    let r = runs((0..4).map(|i| (i, ppu_point(o, i).y())));
    assert_eq!(r, vec![vec![(0, B30 as f64), (1, 0.0), (2, 7.0)]]);
    // A NOT VALID owner (a lost record): every slot REFUSED, no line at all.
    let bad = index(vec![lane(1, seqd(0, [vec![gap(0, 1)], frames_at(&[10, 20, 30])].concat()))]);
    let bo = bad.owner(1).unwrap();
    assert!(bo.slots.iter().all(|s| matches!(s.kind, SlotKind::Refused(_))));
    assert!((0..bo.slots.len()).all(|i| ppu_point(bo, i) == LinePoint::Refused));
    assert!(runs((0..bo.slots.len()).map(|i| (i, ppu_point(bo, i).y()))).is_empty());
}

#[test]
fn runs_break_at_a_missing_value_and_at_an_index_jump() {
    let pts = vec![(0, Some(1.0)), (1, Some(2.0)), (2, None), (3, Some(4.0)), (5, Some(6.0)), (6, Some(7.0))];
    assert_eq!(runs(pts), vec![vec![(0, 1.0), (1, 2.0)], vec![(3, 4.0)], vec![(5, 6.0), (6, 7.0)]]);
    assert!(runs(Vec::<(usize, Option<f64>)>::new()).is_empty());
}

#[test]
fn smoothing_truncates_at_edges_and_breaks_and_never_crosses_a_gap() {
    let v = [Some(1.0), Some(2.0), Some(3.0), None, Some(10.0), Some(20.0), Some(30.0), Some(40.0)];
    let at = |i: usize| v.get(i).copied().flatten();
    let s = smooth(0..8, 3, at);
    assert_eq!(
        s,
        vec![
            Some((1.0, 1)), // the first slot: a window of one, never padded
            Some((1.5, 2)),
            Some((2.0, 3)),
            None, // the gap has no smoothed value
            Some((10.0, 1)), // the run restarts after the gap: nothing carried across
            Some((15.0, 2)),
            Some((20.0, 3)),
            Some((30.0, 3)),
        ]
    );
    // A range starting mid-capture looks back n - 1 slots and no further.
    let mut calls = Vec::new();
    let s = smooth(6..8, 3, |i| {
        calls.push(i);
        at(i)
    });
    assert_eq!(calls, vec![4, 5, 6, 7]);
    assert_eq!(s, vec![Some((20.0, 3)), Some((30.0, 3))]);
    // ... and the lookback stops at the first slot.
    let mut calls = 0;
    let _ = smooth(1..3, 30, |i| {
        calls += 1;
        at(i)
    });
    assert_eq!(calls, 3, "slots 0, 1, 2");
    // A window of 1 is the raw line; an oversized window is capped.
    assert_eq!(smooth(0..3, 1, at), vec![Some((1.0, 1)), Some((2.0, 1)), Some((3.0, 1))]);
    let mut calls = 0;
    let _ = smooth(10_000..10_010, 1_000_000, |_| {
        calls += 1;
        Some(1.0)
    });
    assert_eq!(calls, 10 + SMOOTH_MAX - 1, "the lookback is capped at SMOOTH_MAX - 1");
    assert!(smooth(5..5, 3, at).is_empty());
}

#[test]
fn line_range_is_the_visible_columns_plus_one_neighbour_each_side() {
    assert_eq!(line_range(&(10..20), 100), 9..21);
    assert_eq!(line_range(&(0..20), 20), 0..20);
    assert_eq!(line_range(&(0..0), 20), 0..0);
    assert_eq!(line_range(&(99..100), 100), 98..100);
}

#[test]
fn histogram_is_exact_and_bins_every_measured_value_once() {
    let h = histogram(&[0, 0, 1, 5, 9, 10], 4).unwrap();
    // span 11, width ceil(11 / 4) = 3: [0,3) [3,6) [6,9) [9,12)
    assert_eq!(h, Hist { width: 3, counts: vec![3, 1, 0, 2] });
    assert_eq!(h.total(), 6);
    assert_eq!(histogram(&[], 4), None);
    assert_eq!(histogram(&[1], 0), None);
    // A single value, and the bins never outnumber the values' span.
    assert_eq!(histogram(&[0], 10).unwrap(), Hist { width: 1, counts: vec![1] });
    assert_eq!(histogram(&[0, 2], 10).unwrap(), Hist { width: 1, counts: vec![1, 0, 1] });
    // The largest tick values: one bin 2^64 wide is not truncated.
    let h = histogram(&[0, u64::MAX], 1).unwrap();
    assert_eq!((h.width, h.counts.clone()), (1u128 << 64, vec![2]));
    let h = histogram(&[0, u64::MAX - 1, u64::MAX], 2).unwrap();
    assert_eq!((h.width, h.counts.clone()), (1u128 << 63, vec![1, 2]));
}

#[test]
fn distribution_excludes_unknown_and_refused_and_counts_them() {
    // 25 measured frames (elapsed 100 + i) and the final UNKNOWN frame.
    let mut tbs = vec![1_000u64];
    for i in 0..25u64 {
        tbs.push(tbs.last().unwrap() + 100 + i);
    }
    let ix = index(vec![lane(1, seqd(0, frames_at(&tbs)))]);
    let o = ix.owner(1).unwrap();
    let sm = summary(o, ix.freq_hz, Budget::Fps30);
    assert_eq!((sm.measured, sm.unknown, sm.refused), (25, 1, 0));
    let h = histogram(o.measured_sorted(), 48).unwrap();
    assert_eq!(h.total(), 25, "only measured frames are binned");
    // ZoneStats rank rule: p50 rank ceil(12.5) = 13 -> 112; p90 rank ceil(22.5) = 23 -> 122; p99 rank 25 -> 124.
    assert_eq!(o.percentile_of(50), Metric::Value(112));
    assert_eq!(o.percentile_of(90), Metric::Value(122));
    assert_eq!(o.percentile_of(99), Metric::Value(124));
    // Below 20 measured frames the percentiles are refused with the F1 wording.
    let few = index(vec![lane(1, seqd(0, frames_at(&[0, 10, 20])))]);
    assert_eq!(
        few.owner(1).unwrap().percentile_of(99),
        Metric::Refused("2 measured frames, percentiles need 20".to_string())
    );
}

#[test]
fn lines_top_covers_the_ppu_maximum_the_lanes_and_the_budget() {
    let ix = index(vec![lane(1, seqd(0, frames_at(&[0, 500, 600])))]);
    let o = ix.owner(1).unwrap();
    assert_eq!(lines_top(o, None, None), 500.0);
    assert_eq!(lines_top(o, Some(900.0), None), 900.0);
    assert_eq!(lines_top(o, Some(900.0), Some(2_000.0)), 2_000.0);
    assert_eq!(lines_top(o, Some(f64::INFINITY), None), 1.0, "a non-finite top falls back");
    let zero = index(vec![lane(1, seqd(0, frames_at(&[5, 5, 5])))]);
    assert_eq!(lines_top(zero.owner(1).unwrap(), None, None), 1.0, "all-zero frames: at least 1 tick");
}

#[test]
fn ppu_verdict_cells_follow_the_exact_rule() {
    let t0 = 1_000;
    let ix = index(vec![lane(1, seqd(0, frames_at(&[t0, t0 + B30, t0 + B30 + B30 + 1, t0 + 2 * B30 + 1])))]);
    let o = ix.owner(1).unwrap();
    let c = |i, b| ppu_cell(o, i, ix.freq_hz, b);
    assert_eq!(c(0, Budget::Fps30), Cell::Within, "exactly at the budget is not over");
    assert_eq!(c(1, Budget::Fps30), Cell::Over, "one tick over");
    assert_eq!(c(2, Budget::Fps30), Cell::Within, "a measured 0 is within");
    assert_eq!(c(3, Budget::Fps30), Cell::Unknown);
    assert_eq!(c(0, Budget::Fps60), Cell::Over);
    assert_eq!(c(0, Budget::None), Cell::NoVerdict);
    assert_eq!(ppu_cell(o, 0, None, Budget::Fps30), Cell::NoVerdict, "no clock: no verdict");
    assert_eq!(c(42, Budget::Fps30), Cell::NoInterval);
    let bad = index(vec![lane(1, seqd(0, [vec![gap(0, 1)], frames_at(&[10, 20])].concat()))]);
    assert_eq!(ppu_cell(bad.owner(1).unwrap(), 0, bad.freq_hz, Budget::Fps30), Cell::Refused);
}

#[test]
fn the_first_nine_lane_styles_are_distinct() {
    let s: Vec<(usize, Dash)> = (0..9).map(lane_style).collect();
    let mut u = s.clone();
    u.sort_by_key(|&(c, d)| (c, d as u8));
    u.dedup();
    assert_eq!(u.len(), 9, "{s:?}");
    assert!(s.iter().all(|&(c, _)| c < LANE_COLOURS));
    assert_eq!(lane_style(9), lane_style(0), "then the styles cycle");
}

#[test]
fn the_p99_fit_scale_ignores_one_hitch_and_max_keeps_it() {
    // 100 measured frames: 99 of 100 ticks and one 10_000-tick hitch (frame 50); the final frame is UNKNOWN.
    let mut tbs = vec![0u64];
    for i in 0..100u64 {
        tbs.push(tbs.last().unwrap() + if i == 50 { 10_000 } else { 100 });
    }
    let ix = index(vec![lane(1, seqd(0, frames_at(&tbs)))]);
    let o = ix.owner(1).unwrap();
    // p99: rank ceil(99 * 100 / 100) = 99 -> 100 ticks; the top is 1.1 x that.
    assert!((fit_top(o, &[], None, None, false) - 110.0).abs() < 1e-9);
    assert_eq!(fit_top(o, &[], None, None, true), 10_000.0, "Max: every finite value fits");
    // A lane whose distribution was refused contributes every finite end (the lanes' strip top).
    assert!((fit_top(o, &[Err("refused".to_string())], Some(9_000.0), None, false) - 9_900.0).abs() < 1e-9);
    assert!((fit_top(o, &[], None, Some(500.0), false) - 550.0).abs() < 1e-9, "the budget stays on the axis");
    // Below 20 measured frames the fit falls back to the maximum.
    let few = index(vec![lane(1, seqd(0, frames_at(&[0, 100, 5_100])))]);
    assert!((fit_top(few.owner(1).unwrap(), &[], None, None, false) - 5_500.0).abs() < 1e-9);
}
