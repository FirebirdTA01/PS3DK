//! Frames page view-model tests, on captures from the reader tests' independent writer.  Every expected value is
//! derived by hand beside it; the reader's own Metrics are the authority the model must match.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use common::*;
use ps3dk_prof::{load, Analysis, Metric, Outcome};
use ps3dk_prof_viewer::frames::*;
use ps3dk_prof_viewer::frames_ui::FramesState;
use ps3dk_prof_viewer::model::Document;
use ps3dk_prof_viewer::timeline::{timeline, View};
use std::path::PathBuf;

const F: u64 = 79_800_000; // the scenario CLOCKS frequency
const B30: u64 = F / 30; // 2_660_000 ticks, exact

fn analysed(bytes: &[u8]) -> Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

fn index(recs: Vec<Lane>) -> (Analysis, FramesIndex) {
    let a = analysed(&scenario(recs).bytes());
    let ix = FramesIndex::build(&a);
    (a, ix)
}

fn frames_at(tbs: &[u64]) -> Vec<Rec> {
    tbs.iter().enumerate().map(|(i, &tb)| frame(0, tb, i as u64)).collect()
}

#[test]
fn over_budget_is_exact_and_equality_is_not_over() {
    assert!(!over_budget(B30, 30, F));
    assert!(over_budget(B30 + 1, 30, F));
    assert!(!over_budget(F / 60, 60, F));
    assert!(over_budget(F / 60 + 1, 60, F));
    // A frequency 30 does not divide: 2_660_000 * 30 = 79_800_000 is not above 79_800_001; one more tick is.
    assert!(!over_budget(2_660_000, 30, F + 1));
    assert!(over_budget(2_660_001, 30, F + 1));
    assert!(over_budget(u64::MAX, 60, u64::MAX), "u128: no overflow");
    assert!(!over_budget(0, 60, F));
}

#[test]
fn slots_follow_the_reader_and_the_budget_boundary() {
    // FRAME 0, 1, 2: elapsed exactly the 30 FPS budget, then one tick over; FRAME 2 is the final (unclosed) frame.
    let t0 = 1_000;
    let recs = seqd(0, frames_at(&[t0, t0 + B30, t0 + B30 + B30 + 1]));
    let (a, ix) = index(vec![lane(1, recs)]);
    let o = ix.owner(1).unwrap();
    assert_eq!(o.slots.len(), 3);
    assert_eq!((o.slots[0].kind.clone(), o.slots[0].next_tb), (SlotKind::Measured(B30), Some(t0 + B30)));
    assert_eq!(o.slots[1].kind, SlotKind::Measured(B30 + 1));
    assert!(matches!(&o.slots[2].kind, SlotKind::Unknown(w) if w.contains("unclosed")), "{:?}", o.slots[2]);
    assert_eq!(o.slots[2].next_tb, None, "the final frame has no interval");
    for (s, k) in o.slots.iter().zip(0u64..) {
        let reader = a.elapsed_for(1, k).unwrap();
        match (&s.kind, reader) {
            (SlotKind::Measured(d), Metric::Value(r)) => assert_eq!(d, r),
            (SlotKind::Unknown(w), Metric::Unknown(r)) | (SlotKind::Refused(w), Metric::Refused(r)) => {
                assert_eq!(w, r)
            }
            other => panic!("slot disagrees with the reader: {other:?}"),
        }
    }
    let s30 = summary(o, ix.freq_hz, Budget::Fps30);
    assert_eq!((s30.measured, s30.unknown, s30.refused, s30.over), (2, 1, 0, Some(1)));
    assert_eq!(summary(o, ix.freq_hz, Budget::Fps60).over, Some(2));
    assert_eq!(summary(o, ix.freq_hz, Budget::None).over, None);
    assert_eq!((s30.min, s30.max), (Some(B30), Some(B30 + 1)));
    assert_eq!(s30.p50, Metric::Refused("2 measured frames, percentiles need 20".to_string()));
    let sc = scale(o, ix.freq_hz, Budget::Fps30, 100.0);
    let g: Vec<Glyph> = (0..o.slots.len()).map(|i| glyph(o, i, &sc)).collect();
    let line = sc.line;
    assert!(matches!(g[0], Glyph::Bar { over: false, .. }));
    assert!(matches!(g[1], Glyph::Bar { over: true, .. }));
    assert_eq!(g[2], Glyph::Unknown);
    assert!(line.is_some());
    assert_eq!(scale(o, None, Budget::Fps30, 100.0).line, None, "no clock: no budget line");
    assert!(!matches!(glyph(o, 1, &scale(o, None, Budget::Fps30, 100.0)), Glyph::Bar { over: true, .. }));
}

#[test]
fn zero_elapsed_is_measured_and_distinct_from_unknown() {
    let recs = seqd(0, frames_at(&[500, 500, 900]));
    let (_, ix) = index(vec![lane(1, recs)]);
    let o = ix.owner(1).unwrap();
    assert_eq!((o.slots[0].kind.clone(), o.slots[0].next_tb), (SlotKind::Measured(0), Some(500)));
    let sc = scale(o, ix.freq_hz, Budget::Fps30, 100.0);
    let g: Vec<Glyph> = (0..o.slots.len()).map(|i| glyph(o, i, &sc)).collect();
    assert_eq!(g[0], Glyph::Zero);
    assert_eq!(g[2], Glyph::Unknown);
    assert_ne!(g[0], g[2]);
}

#[test]
fn a_missing_successor_never_borrows_another_frame() {
    // FRAME 0, then lost records (a GAP may hide FRAME 1), then FRAME 2: frame 0's next record is not k + 1, so it
    // is not measured and has no interval (whatever the reader's reason), and FRAME 2 is never borrowed.
    let recs = seqd(0, vec![frame(0, 100, 0), gap(1, 1), frame(0, 200, 2), frame(0, 300, 3)]);
    let (_, ix) = index(vec![lane(1, recs)]);
    let o = ix.owner(1).unwrap();
    assert!(!matches!(o.slots[0].kind, SlotKind::Measured(_)), "{:?}", o.slots[0]);
    assert_eq!(o.slots[0].next_tb, None);
    assert_eq!(o.slots[0].frame, 0);
    assert_eq!(o.slots[1].frame, 2);
}

#[test]
fn a_backward_clock_is_not_measured() {
    let recs = seqd(0, frames_at(&[1_000, 900, 2_000]));
    let a = analysed(&scenario(vec![lane(1, recs)]).bytes());
    let ix = FramesIndex::build(&a);
    let o = ix.owner(1).unwrap();
    assert!(!matches!(o.slots[0].kind, SlotKind::Measured(_)), "{:?}", o.slots[0]);
    assert_eq!(o.slots[0].next_tb, None);
    match &o.slots[0].kind {
        SlotKind::Unknown(w) | SlotKind::Refused(w) => assert!(!w.is_empty()),
        SlotKind::Measured(_) => unreachable!(),
    }
}

#[test]
fn percentiles_need_twenty_measured_frames() {
    // elapsed_i = 1000 + 2i + 1 (ascending): FRAME i at tb_i = 1000 * i + i * i.
    let tbs = |n: u64| (0..n).map(|i| 1_000 * i + i * i).collect::<Vec<_>>();
    let (_, ix) = index(vec![lane(1, seqd(0, frames_at(&tbs(20))))]); // 19 measured + the final frame
    let s = summary(ix.owner(1).unwrap(), ix.freq_hz, Budget::Fps30);
    assert_eq!((s.measured, s.unknown), (19, 1));
    assert_eq!(s.p90, Metric::Refused("19 measured frames, percentiles need 20".to_string()));
    let (_, ix) = index(vec![lane(1, seqd(0, frames_at(&tbs(21))))]); // 20 measured
    let s = summary(ix.owner(1).unwrap(), ix.freq_hz, Budget::Fps30);
    assert_eq!(s.measured, 20);
    // rank(50) = 10 -> the 10th smallest, i = 9: 1019; rank(90) = 18 -> i = 17: 1035.
    assert_eq!((s.p50, s.p90), (Metric::Value(1_019), Metric::Value(1_035)));
    assert_eq!(s.mean, Some((0..20u64).map(|i| 1_001 + 2 * i).sum::<u64>() as f64 / 20.0));
}

/// Owner lane 1, frame 0: zoneA 1100..1400 with child zoneB 1150..1250, zoneA 1500..1600, and zoneB 1900..2100,
/// which crosses FRAME 1 at 2000 (its whole exclusive duration counts in frame 0, its start frame).
/// Exclusive: 200 + 100 + 100 + 200 = 600.
fn hot_lane() -> Lane {
    lane(
        1,
        seqd(
            0,
            [
                vec![frame(0, 1_000, 0)],
                zone(0, 0, 0, 1_100, 1, 0),
                zone(0, 1, 1, 1_150, 2, 1),
                vec![end(0, 1, 1, 1_250, 2), end(0, 0, 0, 1_400, 1)],
                zone(0, 0, 0, 1_500, 3, 0),
                vec![end(0, 0, 0, 1_600, 3)],
                zone(0, 1, 0, 1_900, 4, 0),
                vec![frame(0, 2_000, 1), end(0, 1, 0, 2_100, 4), frame(0, 3_000, 2)],
            ]
            .concat(),
        ),
    )
}

#[test]
fn a_valid_w_table_sums_exactly_to_the_readers_w() {
    let (a, ix) = index(vec![hot_lane()]);
    assert_eq!(a.work_for(1, 0, 1), Some(&Metric::Value(600)));
    let t = frame_tables(&a, &ix, 1, 0);
    assert_eq!(t.len(), 1);
    let Table::Measured { w, rows } = &t[0].table else { panic!("{:?}", t[0]) };
    assert_eq!(*w, 600);
    let got: Vec<(u32, u64, u128)> = rows.iter().map(|r| (r.desc, r.spans, r.exclusive)).collect();
    // zoneA: 200 + 100; zoneB: 100 + 200; tied at 300, ordered by descriptor index.
    assert_eq!(got, vec![(0, 2, 300), (1, 2, 300)]);
    assert_eq!(rows.iter().map(|r| r.exclusive).sum::<u128>(), 600);
    assert!(rows[0].label.starts_with("zoneA") && rows[0].label.contains(':'), "{}", rows[0].label);
    assert_eq!(t[0].counts.frame, KindCounts { completed: 4, censored: 0, unavailable: 0 });
    assert_eq!(t[0].counts.total(), 4);
    // Frame 1 has no claimed span: no table, never a measured 0.
    assert!(frame_tables(&a, &ix, 1, 1).is_empty());
    assert!(ix.lanes_of(1, 1).is_empty());
}

#[test]
fn same_name_descriptors_stay_separate_rows() {
    let mut s = scenario(vec![hot_lane()]);
    s.descs = vec![desc(0, "zoneA", ""), desc(1, "zoneA", ""), desc(2, "zoneC", "")];
    let a = analysed(&s.bytes());
    let ix = FramesIndex::build(&a);
    let t = frame_tables(&a, &ix, 1, 0);
    let Table::Measured { rows, .. } = &t[0].table else { panic!() };
    assert_eq!(rows.len(), 2, "two descriptors, two rows, despite the same name");
}

#[test]
fn an_invalid_worker_lane_shows_no_number() {
    // Owner 1 is valid; worker 2 lost records (a GAP: NOT VALID) and its tokened span claims owner 1 frame 0.
    let owner = lane(1, seqd(0, frames_at(&[1_000, 2_000, 3_000])));
    let worker = lane(
        2,
        seqd(0, [vec![gap(0, 1)], zone_tok(0, 0, 0, 1_100, 1, 0, 1, 0), vec![end(0, 0, 0, 1_300, 1)]].concat()),
    );
    let (a, ix) = index(vec![owner, worker]);
    assert!(a.lane_valid(1) && !a.lane_valid(2));
    assert!(matches!(a.work_for(1, 0, 2), Some(Metric::Refused(r)) if r.contains("lane invalid")));
    let t = frame_tables(&a, &ix, 1, 0);
    let w2 = t.iter().find(|t| t.lane == 2).expect("the worker lane has a Work entry");
    assert!(matches!(&w2.table, Table::Refused(r) if r.contains("lane invalid")), "{:?}", w2.table);
    assert_eq!(w2.counts.total(), 1);
}

#[test]
fn a_censored_span_refuses_the_whole_lane_table() {
    // zoneA 1100..1200 completes; zoneB opens at 1300 and never ends (censored).
    let recs = seqd(
        0,
        [
            vec![frame(0, 1_000, 0)],
            zone(0, 0, 0, 1_100, 1, 0),
            vec![end(0, 0, 0, 1_200, 1)],
            zone(0, 1, 0, 1_300, 2, 0),
            vec![frame(0, 2_000, 1)],
        ]
        .concat(),
    );
    let (a, ix) = index(vec![lane(1, recs)]);
    assert!(matches!(a.work_for(1, 0, 1), Some(Metric::Refused(r)) if r.contains("censored")));
    let t = frame_tables(&a, &ix, 1, 0);
    assert!(matches!(&t[0].table, Table::Refused(r) if r.contains("censored")), "{:?}", t[0].table);
    assert_eq!(t[0].counts.frame, KindCounts { completed: 1, censored: 1, unavailable: 0 });
}

#[test]
fn unattributed_spans_are_capture_wide_and_never_frame_exclusions() {
    // A span before the owner's first FRAME (PreFrame) and a lane with no FRAME at all (NotOwner).
    let owner = lane(
        1,
        seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)], frames_at(&[1_000, 2_000])].concat()),
    );
    let other = lane(2, seqd(0, [zone(0, 1, 0, 1_100, 1, 0), vec![end(0, 1, 0, 1_200, 1)]].concat()));
    let (a, ix) = index(vec![owner, other]);
    assert_eq!(ix.unattributed.get(&1).map(|u| u.pre_frame), Some(1));
    assert_eq!(ix.unattributed.get(&2).map(|u| u.not_owner), Some(1));
    assert!(frame_tables(&a, &ix, 1, 0).is_empty(), "nothing is claimed by frame 0");
}

#[test]
fn index_keys_are_exactly_the_readers_work_keys() {
    let owner = lane(1, seqd(0, frames_at(&[1_000, 2_000, 3_000])));
    let worker = lane(
        2,
        seqd(0, [vec![gap(0, 1)], zone_tok(0, 0, 0, 1_100, 1, 0, 1, 0), vec![end(0, 0, 0, 1_300, 1)]].concat()),
    );
    for lanes in [vec![hot_lane()], vec![owner, worker]] {
        let (a, ix) = index(lanes);
        let mine: Vec<(u32, u64, u32)> = ix.work_keys().copied().collect();
        let reader: Vec<(u32, u64, u32)> = a.work.iter().map(|w| (w.owner, w.frame, w.lane)).collect();
        assert_eq!(mine, reader);
    }
}

#[test]
fn no_owner_and_all_invalid_owners() {
    let (_, ix) = index(vec![lane(1, seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat()))]);
    assert!(ix.owners.is_empty());
    assert_eq!(ix.default_owner(), None);
    // The only owner lost records: NOT VALID, every slot REFUSED, the lowest owner still selected, no stats.
    let recs = seqd(0, vec![frame(0, 1_000, 0), gap(1, 1), frame(0, 3_000, 2), frame(0, 4_000, 3)]);
    let (_, ix) = index(vec![lane(3, recs)]);
    let o = &ix.owners[0];
    assert!(!o.valid && !o.reasons.is_empty());
    assert!(o.slots.iter().all(|s| matches!(s.kind, SlotKind::Refused(_))), "{:?}", o.slots);
    assert_eq!(ix.default_owner(), Some(3));
    let s = summary(o, ix.freq_hz, Budget::Fps30);
    assert_eq!((s.measured, s.over, s.min, s.mean), (0, None, None, None));
}

#[test]
fn a_small_interval_near_two_to_the_64_stays_exact() {
    let t = u64::MAX - 10_000;
    let (a, ix) = index(vec![lane(1, seqd(0, frames_at(&[t, t + 5, t + 9])))]);
    let o = ix.owner(1).unwrap();
    assert_eq!((o.slots[0].kind.clone(), o.slots[0].next_tb), (SlotKind::Measured(5), Some(t + 5)));
    let tl = timeline(&a);
    assert_eq!(tl.rel(t + 5) - tl.rel(t), 5.0, "the integer origin is removed before the float");
    let mut v = View::fit(&tl);
    v.show_range(&tl, t, t + 5);
    assert!(v.t0 <= tl.rel(t) && v.t0 + v.dt >= tl.rel(t + 5), "the measured interval is framed: {v:?}");
}

#[test]
fn selection_keeps_identity_or_resets() {
    let (_, a2) = index(vec![lane(1, seqd(0, frames_at(&[10, 20, 30]))), lane(2, seqd(0, frames_at(&[10, 20])))]);
    let fresh = Selection::fresh(&a2);
    assert_eq!(fresh, Selection { owner: Some(1), frame: None });
    let kept = Selection { owner: Some(2), frame: Some(1) }.refreshed(&a2);
    assert_eq!(kept, Selection { owner: Some(2), frame: Some(1) });
    let gone_frame = Selection { owner: Some(2), frame: Some(7) }.refreshed(&a2);
    assert_eq!(gone_frame, Selection { owner: Some(2), frame: None });
    // Owner 9 disappeared: the default owner, and the frame is cleared even though owner 1 also has a frame 1.
    let gone_owner = Selection { owner: Some(9), frame: Some(1) }.refreshed(&a2);
    assert_eq!(gone_owner, Selection { owner: Some(1), frame: None });
}

#[test]
fn document_lifecycle_rebuilds_or_clears_the_frames_state() {
    let doc = |bytes: &[u8]| Document::from_bytes(PathBuf::from("t.prof"), bytes);
    let d = doc(&scenario(vec![hot_lane()]).bytes());
    let (a, ix) = (d.analysis().unwrap(), d.frames.as_ref().unwrap());
    let mut st = FramesState::default();
    st.reset(Some(ix));
    st.select(a, ix, 1, Some(0));
    assert!(matches!(&st.tables, Some(((1, 0), t)) if t.len() == 1));
    // A newer snapshot of the same capture keeps (owner, frame) and rebuilds the tables from the new document.
    let d2 = doc(&scenario(vec![hot_lane()]).bytes());
    st.refresh(d2.analysis().unwrap(), d2.frames.as_ref().unwrap());
    assert_eq!(st.sel, Selection { owner: Some(1), frame: Some(0) });
    assert!(st.tables.is_some());
    // A MALFORMED replacement: no frame index, the selection is cleared.
    let mut bad = scenario(vec![hot_lane()]).bytes();
    bad[0] ^= 0xff;
    let dm = doc(&bad);
    assert!(dm.frames.is_none());
    st.reset(dm.frames.as_ref());
    assert_eq!((st.sel, st.tables.is_none()), (Selection::default(), true));
    // A v1 capture is fully supported (frames are PPU facts); a fresh document starts at the default owner.
    let dv = doc(&scenario(vec![lane(4, seqd(0, frames_at(&[1, 2])))]).bytes());
    st.reset(dv.frames.as_ref());
    assert_eq!(st.sel, Selection { owner: Some(4), frame: None });
    // The budget choice survives documents; the default is 30 FPS.
    assert_eq!(FramesState::default().budget(), Budget::Fps30);
}

#[test]
fn owner_invalid_and_unresolved_spans_are_counted_once_without_numbers() {
    // Owner 1 lost records between FRAME 0 and FRAME 2 (NOT VALID): its own span in frame 0 is OwnerInvalid.
    // Worker 2 (valid) holds a token for frame 1, which fell in the owner's GAP.
    let mut recs = vec![frame(0, 1_000, 0)];
    recs.extend(zone(1, 0, 0, 1_100, 1, 0));
    recs.extend([end(3, 0, 0, 1_200, 1), gap(4, 4), frame(5, 3_000, 2), frame(6, 4_000, 3)]);
    let owner = lane(1, recs);
    let worker = lane(2, seqd(0, [zone_tok(0, 1, 0, 2_100, 1, 0, 1, 1), vec![end(0, 1, 0, 2_300, 1)]].concat()));
    let (a, ix) = index(vec![owner, worker]);
    let t0 = frame_tables(&a, &ix, 1, 0);
    let own = t0.iter().find(|t| t.lane == 1).expect("the owner's own span claims frame 0");
    assert_eq!(own.counts.owner_invalid, KindCounts { completed: 1, censored: 0, unavailable: 0 });
    assert_eq!(own.counts.total(), 1, "counted once");
    assert!(matches!(&own.table, Table::Refused(r) if r.contains("owner invalid")), "{:?}", own.table);
    let t1 = frame_tables(&a, &ix, 1, 1);
    if let Some(w) = t1.iter().find(|t| t.lane == 2) {
        assert_eq!(w.counts.total(), 1, "counted once: {:?}", w.counts);
        assert!(matches!(w.table, Table::Refused(_)), "{:?}", w.table);
        assert_eq!(w.counts.frame.completed, 0, "a span on a lost frame is not attributed: {:?}", w.counts);
    } else {
        panic!("the worker's token claims frame 1: {:?}", a.spans);
    }
}

#[test]
fn visible_work_is_bounded_on_a_large_owner() {
    // 100000 slots of 6 px: a 1200 px viewport asks for at most 1200 / 6 + 2 slots at either end.
    let (n, w) = (100_000usize, 6.0);
    let total = n as f64 * w;
    let first = visible_slots(n, w, 0.0, 1_200.0);
    assert_eq!(first, 0..200);
    let last = visible_slots(n, w, total - 1_200.0, total);
    assert_eq!(last, n - 200..n);
    let odd = visible_slots(n, w, 3.0, 1_203.0); // a viewport that cuts columns
    assert_eq!(odd, 0..201);
    assert!(odd.len() <= 1_200 / 6 + 2);
    assert_eq!(visible_slots(n, w, total + 10.0, total + 1_210.0), n..n, "past the end: nothing");
    // Absolute identity under scrolling: the column at x is x / w, independent of the viewport.
    assert_eq!(slot_at(n, w, 0.0), Some(0));
    assert_eq!(slot_at(n, w, 599_999.0), Some(99_999));
    assert_eq!(slot_at(n, w, 600_000.0), None);
    assert_eq!(slot_at(n, w, -0.5), None);
}

#[test]
fn a_large_owner_indexes_once_and_summarises_from_its_cache() {
    // 100001 FRAME records: 100000 measured slots of 1000 ticks and the final unknown one.
    let a = analysed(&chunked_frames(100_000));
    let ix = FramesIndex::build(&a);
    let o = ix.owner(1).unwrap();
    assert_eq!(o.slots.len(), 100_001);
    let s = summary(o, ix.freq_hz, Budget::Fps30);
    assert_eq!((s.measured, s.unknown, s.over, s.p50.clone()), (100_000, 1, Some(0), Metric::Value(1_000)));
    // Every slot equals the reader's own Metric (the keyed lookup, not a scan, built it).
    for (sl, e) in o.slots.iter().zip(a.frames.iter().filter(|e| e.owner == 1)) {
        match (&sl.kind, &e.value) {
            (SlotKind::Measured(d), Metric::Value(r)) => assert_eq!(d, r),
            (SlotKind::Unknown(x), Metric::Unknown(y)) => assert_eq!(x, y),
            other => panic!("{other:?}"),
        }
    }
    assert_eq!(o.slot_of(77_777).map(|(i, s)| (i, s.frame)), Some((77_777, 77_777)));
    assert_eq!(o.slot_of(200_000), None);
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
