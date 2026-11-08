//! F2b (t_d9789c06): the Frames page SPU/RSX lanes' view model, through the real reader on parsed captures, plus
//! handcrafted reader results for the scale, verdict and text boundaries the reader cannot reach from one capture.

#[path = "../../ps3dk-prof/tests/common/mod.rs"]
#[rustfmt::skip]
#[allow(clippy::all)]
mod common;

use std::collections::BTreeMap;

use common::*;
use num_bigint::BigInt;
use ps3dk_prof::clockmap::Rat;
use ps3dk_prof::durations::Bound;
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::frames_foreign::{
    EpochF2, EpochRow, ForeignFrames, FrameCell, FrameSet, Interval, LaneCounts, LaneF2, PlacementState, Refusal,
    ZoneCell,
};
use ps3dk_prof::{Analysis, Outcome};
use ps3dk_prof_viewer::foreign::{
    bound_exact, bound_rounded, build, epoch_text, mark, ms_exact, refusal_text, ticks_exact, verdict, ForeignSlot,
    Labels, Mark, VerdictView, ROUNDED,
};
use ps3dk_prof_viewer::frames::{Budget, FramesIndex};

const T_DOMAIN: u32 = 8;
const T_EPOCH: u32 = 9;
const T_CAL: u32 = 10;
const T_IMAGES: u32 = 11;
const T_LANES: u32 = 12;
const T_SPU: u32 = 13;
const T_GPU: u32 = 14;
const T_FINAL: u32 = 16;
const LANE: u32 = 10;
const SPU_D: u32 = 2;
const RSX_D: u32 = 3;
const GPU_DESC: u32 = 3;
/// The scenario writer's CLOCKS frequency.
const FREQ: u64 = 79_800_000;

// ---- a test-only extension writer (the format of ps3dk-prof/tests/ext.rs) ----------------------------------------

#[allow(clippy::too_many_arguments)]
fn domain_entry(b: &mut Buf, id: u32, kind: u8, width: u8, dir: u8, source_kind: u32, source_ref: u32, name: &str) {
    b.u32(id).u8(kind).u8(width).u8(dir).u8(0).u32(source_kind).u32(source_ref).u64(0).u64(0).u64(0).u64(0).str(name);
}
fn domains(entries: &[(u32, u8, u32)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(id, kind, r) in entries {
        if kind == 1 {
            domain_entry(&mut b, id, 1, 32, 1, 1, r, "spu");
        } else {
            domain_entry(&mut b, id, 2, 64, 2, 2, r, "rsx");
        }
    }
    chunk(T_DOMAIN, b.0)
}
fn epochs(entries: &[(u32, u32, u8, u64)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(d, e, reason, wv) in entries {
        b.u32(d).u32(e).u8(reason).u8(0).u8(0).u8(0).u64(wv).u64(5000 + u64::from(e));
    }
    chunk(T_EPOCH, b.0)
}
fn calibration(d: u32, e: u32, first: u64, brackets: &[(u64, u64, u64)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(d).u32(e).u64(first).u32(brackets.len() as u32);
    for &(f, t0, t1) in brackets {
        b.u64(f).u64(t0).u64(t1);
    }
    chunk(T_CAL, b.0)
}
/// Images with named descriptors: (index, names).
fn images(imgs: &[(u32, &[&str])]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(imgs.len() as u32);
    for &(index, names) in imgs {
        b.u32(index).bytes_str(format!("build-{index}").as_bytes()).u32(names.len() as u32);
        for (i, n) in names.iter().enumerate() {
            let i = i as u32;
            b.u32(i).u32(0x100 + i).u32(10 + i).str(n).str("spu/main.c");
        }
    }
    chunk(T_IMAGES, b.0)
}
fn lanes(entries: &[(u32, u32, u32, u32)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(l, d, img, owner) in entries {
        b.u32(l).u32(d).u32(img).u32(owner).u64(77).str("worker");
    }
    chunk(T_LANES, b.0)
}
fn spu_events(lane: u32, e: u32, first: u64, ev: &[(u32, u16, u8, u8)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(lane).u32(e).u64(first).u32(ev.len() as u32);
    for &(counter, desc, kind, depth) in ev {
        b.u32(counter).u16(desc).u8(kind).u8(depth);
    }
    chunk(T_SPU, b.0)
}
fn gpu_events(d: u32, e: u32, first: u64, ev: &[(u64, u32, u8, u8, u32, u64)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(d).u32(e).u64(first).u32(ev.len() as u32);
    for &(raw, desc, kind, depth, owner, frame) in ev {
        b.u64(raw).u32(desc).u8(kind).u8(depth).u16(0).u32(owner).u64(frame);
    }
    chunk(T_GPU, b.0)
}
fn finals(entries: &[(u32, u8, u8, u64, u64, u64, u32)]) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(s, k, st, next, ev, lost, open) in entries {
        b.u32(s).u8(k).u8(st).u16(0).u64(next).u64(ev).u64(lost).u32(open);
    }
    chunk(T_FINAL, b.0)
}

/// One SPU lane (one zone) and one RSX domain, as ps3dk-prof's cm_ext: SPU B = [1/10, 3/10] under W32, RSX B =
/// [29/300, 31/300]; the SPU pair (d = 10) begins in PPU frame 0, the RSX pair (d = 1000) in frame 1.
fn cm_ext() -> Vec<Chunk> {
    vec![
        domains(&[(SPU_D, 1, LANE), (RSX_D, 2, 0)]),
        images(&[(0, &["spuzone0", "spuzone1"])]),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 1, 1, 0)]),
        calibration(SPU_D, 1, 0, &[(1000, 10, 20), (900, 30, 40)]),
        calibration(RSX_D, 1, 0, &[(4000, 100, 110), (7000, 400, 410)]),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0)]),
        gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 1, 0), (6_000, GPU_DESC, 2, 0, 1, 0)]),
        finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]),
    ]
}

/// PPU owner 1 with FRAMEs at tb 0, 150 and 300 (measured [0, 150) and [150, 300)), plus `ext`.
fn f2_bytes(ext: Vec<Chunk>) -> Vec<u8> {
    let mut s = scenario(vec![lane(
        1,
        seqd(
            0,
            [
                vec![frame(0, 0, 0)],
                zone(0, 0, 0, 110, 1, 0),
                vec![end(0, 0, 0, 120, 1), frame(0, 150, 1), frame(0, 300, 2)],
            ]
            .concat(),
        ),
    )]);
    let mut gd = desc(GPU_DESC, "gpuzone", "");
    gd.scope = 2;
    s.descs.push(gd);
    let mut f = s.file();
    let at = f.find(T_TRAILER);
    for (i, c) in ext.into_iter().enumerate() {
        f.chunks.insert(at + i, c);
    }
    f.flags = 1;
    f.bytes()
}

fn analysed(bytes: &[u8]) -> Analysis {
    match load(bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("unexpected malformed: {m}"),
    }
}

fn rat(n: i64, d: i64) -> Rat {
    Rat { num: BigInt::from(n), den: BigInt::from(d) }
}
fn bnd(lo: Rat, lo_open: bool, hi: Option<Rat>) -> Bound {
    Bound { lo, lo_open, hi }
}

// ---- exact text and outward rounding -----------------------------------------------------------------------------

#[test]
fn exact_ticks_and_ms() {
    assert_eq!(ticks_exact(&rat(7, 2)), "7/2 ticks");
    assert_eq!(ticks_exact(&rat(8, 2)), "4 ticks");
    assert_eq!(ms_exact(&rat(1, 1), 1000), "1 ms");
    assert_eq!(ms_exact(&rat(1, 1), 3000), "1/3 ms");
    assert_eq!(ms_exact(&rat(1, 1), 8000), "0.125 ms");
    // terminating, but past 6 places: printed as the fraction
    assert_eq!(ms_exact(&rat(1, 1), 128_000_000), "1/128000 ms");
    let b = bnd(rat(290, 3), false, Some(rat(310, 3)));
    assert_eq!(bound_exact(&b, None), "[290/3 ticks, 310/3 ticks]");
    assert_eq!(bound_exact(&b, Some(1000)), "[290/3 ticks, 310/3 ticks] = [290/3 ms, 310/3 ms]");
}

#[test]
fn outward_rounding_encloses_and_says_so() {
    // integers: no rounding, no marker
    assert_eq!(bound_rounded(&bnd(rat(3, 1), false, Some(rat(5, 1))), None), "[3, 5] ticks");
    // ticks: lo down, hi up
    let r = bound_rounded(&bnd(rat(7, 2), false, Some(rat(9, 2))), None);
    assert_eq!(r, format!("[3, 5] ticks ({ROUNDED})"));
    // ms at the x.xxx5 boundary (freq 1 MHz: 12345/10 ticks = 1.2345 ms): lo 1.234, hi 1.235
    let r = bound_rounded(&bnd(rat(12345, 10), false, Some(rat(12345, 10))), Some(1_000_000));
    assert_eq!(r, format!("[1.234, 1.235] ms ({ROUNDED})"));
    // exact at 0.001 ms resolution: no marker
    assert_eq!(bound_rounded(&bnd(rat(1234, 1), false, Some(rat(1235, 1))), Some(1_000_000)), "[1.234, 1.235] ms");
    // open lower end at 0 and an infinite upper end keep their brackets
    assert_eq!(bound_rounded(&bnd(rat(0, 1), true, None), None), "(0, +inf) ticks");
}

// ---- verdicts ----------------------------------------------------------------------------------------------------

#[test]
fn verdict_boundaries_and_refusals() {
    // freq 60, 30 FPS: the budget is exactly 2 ticks
    let v = |lo: Rat, hi: Option<Rat>| verdict(&bnd(lo, false, hi), Some(60), Budget::Fps30);
    assert_eq!(v(rat(2001, 1000), Some(rat(3, 1))), VerdictView::Over, "just above the budget");
    assert_eq!(v(rat(2, 1), Some(rat(3, 1))), VerdictView::Undetermined, "lo exactly at the budget is not OVER");
    assert_eq!(v(rat(1, 1), Some(rat(2, 1))), VerdictView::Within, "hi exactly at the budget is WITHIN");
    assert_eq!(v(rat(1, 1), Some(rat(2001, 1000))), VerdictView::Undetermined, "hi just above");
    assert_eq!(v(rat(1, 1), None), VerdictView::Undetermined, "+inf is never WITHIN");
    assert_eq!(verdict(&bnd(rat(0, 1), true, None), Some(60), Budget::Fps30), VerdictView::Undetermined);
    assert_eq!(verdict(&bnd(rat(1, 1), false, Some(rat(1, 1))), Some(60), Budget::None), VerdictView::NoBudget);
    assert_eq!(verdict(&bnd(rat(1, 1), false, Some(rat(1, 1))), None, Budget::Fps30), VerdictView::NoClock);
    assert_eq!(verdict(&bnd(rat(1, 1), false, Some(rat(1, 1))), Some(0), Budget::Fps30), VerdictView::NoClock);
    // a > 512-bit numerator: the exact comparison is refused, shown as unavailable, never WITHIN
    let huge = Rat { num: BigInt::from(1) << 600u32, den: BigInt::from(1) };
    let r = verdict(&bnd(rat(1, 1), false, Some(huge)), Some(60), Budget::Fps30);
    assert_eq!(r, VerdictView::Unavailable("bits".to_string()));
    assert!(r.text().contains("verdict unavailable"));
}

// ---- labels ------------------------------------------------------------------------------------------------------

#[test]
fn zone_labels_by_source_kind_with_a_namespace_collision() {
    let mut ext = cm_ext();
    ext[1] = images(&[(0, &["spu-a"]), (1, &["spu-b"])]);
    let a = analysed(&f2_bytes(ext));
    let l = Labels::build(&a);
    assert_eq!(l.zone(&a, None, 0), "desc 0 zoneA", "PPU/GPU namespace");
    assert_eq!(l.zone(&a, Some(0), 0), "image 0 desc 0 spu-a");
    assert_eq!(l.zone(&a, Some(1), 0), "image 1 desc 0 spu-b");
    assert_eq!(l.zone(&a, None, GPU_DESC), "desc 3 gpuzone");
    assert_eq!(l.zone(&a, Some(9), 0), "image 9 desc 0 (not published)", "missing image");
    assert_eq!(l.zone(&a, Some(0), 5), "image 0 desc 5 (not published)", "missing descriptor");
    assert_eq!(l.zone(&a, None, 77), "desc 77 (not published)");
}

// ---- the view through the real reader ----------------------------------------------------------------------------

#[test]
fn w32_off_shows_ambiguous_lift_and_on_places_spu() {
    let a = analysed(&f2_bytes(cm_ext()));
    let ix = FramesIndex::build(&a);
    let slot = ForeignSlot::default();
    {
        let v = slot.view(&a, &ix, 1, false, Budget::Fps30);
        let spu = v.lanes.iter().find(|l| l.label == "SPU lane 10").expect("SPU lane");
        assert!(spu.epochs.iter().any(|e| e.contains("AMBIGUOUS-LIFT") && e.contains("coverage unknown")), "{spu:?}");
        assert!(spu.cells.is_empty());
        let rsx = v.lanes.iter().find(|l| l.label == "RSX domain 3").expect("RSX lane");
        assert!(rsx.cells.contains_key(&1), "the RSX pair lands on owner slot 1: {rsx:?}");
    }
    let v = slot.view(&a, &ix, 1, true, Budget::Fps30);
    let spu = v.lanes.iter().find(|l| l.label == "SPU lane 10").unwrap();
    let c = &spu.cells[&0];
    assert_eq!(c.bar_exact.as_deref(), Some("[1 ticks, 3 ticks] = [1/79800 ms, 1/26600 ms]"));
    let b = c.bar.as_ref().unwrap();
    assert_eq!(b.verdict, VerdictView::Within);
    assert_eq!(b.label, format!("[0.000, 0.001] ms ({ROUNDED})"));
    assert_eq!(c.rows[0].label, "image 0 desc 0 spuzone0");
    let rsx = v.lanes.iter().find(|l| l.label == "RSX domain 3").unwrap();
    assert_eq!(rsx.cells[&1].rows[0].label, "desc 3 gpuzone");
    // the footer labels the placed-epoch scope of the partition
    assert!(spu.footer[0].starts_with("placed epochs only: 1 pairs"), "{:?}", spu.footer);
    assert!(spu.footer.iter().any(|f| f.starts_with("flags (may overlap)")));
}

#[test]
fn marks_none_placed_vs_no_interval() {
    let a = analysed(&f2_bytes(cm_ext()));
    let ix = FramesIndex::build(&a);
    let o = ix.owner(1).unwrap();
    let slot = ForeignSlot::default();
    let v = slot.view(&a, &ix, 1, true, Budget::Fps30);
    let rsx = v.lanes.iter().find(|l| l.label == "RSX domain 3").unwrap();
    assert_eq!(mark(o, rsx, 0), Mark::NonePlaced, "measured frame 0, nothing included on the RSX lane");
    assert!(matches!(mark(o, rsx, 1), Mark::Bar(_)));
    assert_eq!(mark(o, rsx, 2), Mark::NoInterval, "the final FRAME has no interval");
    assert_eq!(mark(o, rsx, 99), Mark::NoInterval);
}

#[test]
fn cache_counters_move_only_on_real_changes() {
    let a = analysed(&f2_bytes(cm_ext()));
    let ix = FramesIndex::build(&a);
    let slot = ForeignSlot::default();
    let n = |s: &ForeignSlot| s.counters.get();
    for _ in 0..100 {
        drop(slot.view(&a, &ix, 1, false, Budget::Fps30));
    }
    assert_eq!(n(&slot), (1, 1), "100 repaints: one reader pass, one view build");
    drop(slot.view(&a, &ix, 1, false, Budget::Fps60));
    assert_eq!(n(&slot), (1, 2), "a budget change rebuilds only the view");
    drop(slot.view(&a, &ix, 1, true, Budget::Fps60));
    assert_eq!(n(&slot), (2, 3), "the W32 toggle reruns the reader once");
    drop(slot.view(&a, &ix, 99, true, Budget::Fps60));
    assert_eq!(n(&slot), (3, 4), "an owner change reruns the reader once (a refusal is cached too)");
    {
        let v = slot.view(&a, &ix, 99, true, Budget::Fps60);
        assert_eq!(v.refusal.as_deref(), Some(refusal_text(&Refusal::NotAnOwner).as_str()));
    }
    assert_eq!(n(&slot), (3, 4), "the cached refusal is reused");
    let fresh = ForeignSlot::default();
    assert_eq!(n(&fresh), (0, 0), "a new Document starts empty");
}

// ---- handcrafted reader results: scale, model error, words -------------------------------------------------------

fn cell(frame: u64, bar: Option<Bound>) -> FrameCell {
    FrameCell {
        frame,
        included: u64::from(bar.is_some()),
        zero_delta: 0,
        bar_epochs: u64::from(bar.is_some()),
        zones: match &bar {
            Some(b) => BTreeMap::from([((None, GPU_DESC), ZoneCell { pairs: 1, bound: b.clone(), epochs: 1 })]),
            None => BTreeMap::new(),
        },
        bar,
        may_belong: 0,
        may_belong_beyond: 0,
        excluded_extrapolated: 0,
        excluded_exclusive_unavailable: 0,
    }
}

fn handcrafted(cells: Vec<FrameCell>) -> ForeignFrames {
    ForeignFrames {
        frames: FrameSet::new(1, vec![Interval { frame: 0, s: 0, e: 150 }, Interval { frame: 1, s: 150, e: 300 }]).unwrap(),
        state: PlacementState::Placed,
        lanes: vec![LaneF2 {
            source: SourceKey::Gpu(RSX_D),
            epochs: vec![EpochRow { epoch: 1, state: EpochF2::Placed, pairs: Some(cells.len() as u64) }],
            frames: cells,
            counts: LaneCounts::default(),
        }],
        exhausted_at: None,
        used: 0,
    }
}

#[test]
fn scale_covers_a_positive_lower_end_under_an_infinite_upper_end() {
    let a = analysed(&f2_bytes(cm_ext()));
    let ix = FramesIndex::build(&a);
    let l = Labels::build(&a);
    let budget_ticks = FREQ as f64 / 30.0;
    // frame 0: lo above every finite hi and the 30 FPS budget, hi = +inf; frame 1: a small finite bar
    let lo = (budget_ticks as i64) * 2;
    let r = Ok(handcrafted(vec![cell(0, Some(bnd(rat(lo, 1), false, None))), cell(1, Some(bnd(rat(1, 1), false, Some(rat(2, 1)))))]));
    let v = build(&a, &ix, 1, &r, &l, Budget::Fps30);
    assert_eq!(v.top, lo as f64, "the top includes the finite lower end of the +inf bar");
    let b = v.lanes[0].cells[&0].bar.as_ref().unwrap();
    assert!(b.hi_inf && b.hi.is_none() && b.lo == lo as f64);
    assert_eq!(b.verdict, VerdictView::Over);
    // nothing finite and positive: the 1-tick fallback (no budget, all zero)
    let r = Ok(handcrafted(vec![cell(0, Some(bnd(rat(0, 1), false, Some(rat(0, 1)))))]));
    assert_eq!(build(&a, &ix, 1, &r, &l, Budget::None).top, 1.0);
}

#[test]
fn a_cell_off_the_owner_slots_is_a_shown_model_error() {
    let a = analysed(&f2_bytes(cm_ext()));
    let ix = FramesIndex::build(&a);
    let r = Ok(handcrafted(vec![cell(42, Some(bnd(rat(1, 1), false, Some(rat(2, 1)))))]));
    let v = build(&a, &ix, 1, &r, &Labels::build(&a), Budget::Fps30);
    assert!(v.lanes[0].cells.is_empty());
    assert_eq!(v.lanes[0].errors, vec!["frame 42 is not a slot of owner 1 (model error)".to_string()]);
}

#[test]
fn every_refusal_and_epoch_state_has_distinct_words() {
    let refusals = [
        Refusal::NotAnOwner,
        Refusal::OwnerInvalid("why".to_string()),
        Refusal::NoExtension,
        Refusal::TooManyFrames,
        Refusal::FrameOrder,
        Refusal::RetentionModels,
        Refusal::RetentionPairs,
        Refusal::Inputs,
        Refusal::CaptureOps,
    ];
    let t: Vec<String> = refusals.iter().map(refusal_text).collect();
    let mut u = t.clone();
    u.sort();
    u.dedup();
    assert_eq!(u.len(), t.len(), "{t:?}");
    use ps3dk_prof::clockmap::Limit;
    use ps3dk_prof::durations::DLimit;
    use ps3dk_prof::frames_foreign::FLimit;
    let states = [
        EpochF2::Placed,
        EpochF2::NotPlaced,
        EpochF2::AmbiguousLift,
        EpochF2::ClockUnsupported(Limit::Capture),
        EpochF2::NoObservations,
        EpochF2::Inconsistent,
        EpochF2::DurationUnsupported(DLimit::Pairs),
        EpochF2::OrderContradicted,
        EpochF2::Unsupported(FLimit::EpochOps),
    ];
    let t: Vec<String> = states.iter().map(|s| epoch_text(&EpochRow { epoch: 1, state: s.clone(), pairs: Some(2) })).collect();
    let mut u = t.clone();
    u.sort();
    u.dedup();
    assert_eq!(u.len(), t.len(), "{t:?}");
    assert!(t.iter().skip(1).all(|s| s.contains("coverage unknown")), "every state but PLACED says coverage unknown");
    assert!(!t[0].contains("coverage unknown"));
}

// ---- review 12:24 carries: the no-clock draw path, excluded/candidate-only cells, NoInterval slot kinds -----------

/// Owner 1 with FRAMEs at `tbs` (ids 0..) and an optional lost record (owner NOT VALID).
fn owner_bytes(tbs: &[u64], gap: bool, ext: Vec<Chunk>) -> Vec<u8> {
    let mut recs = Vec::new();
    if gap {
        recs.push(common::gap(0, 0));
    }
    for (k, &tb) in tbs.iter().enumerate() {
        recs.push(frame(0, tb, k as u64));
    }
    let mut s = scenario(vec![lane(1, seqd(0, recs))]);
    let mut gd = desc(GPU_DESC, "gpuzone", "");
    gd.scope = 2;
    s.descs.push(gd);
    let mut f = s.file();
    let at = f.find(T_TRAILER);
    for (i, c) in ext.into_iter().enumerate() {
        f.chunks.insert(at + i, c);
    }
    f.flags = 1;
    f.bytes()
}

/// Through the page's own draw call: without a clock the page passes Budget::None, and the RSX bar's verdict is
/// "verdict unavailable (no clock)", never "no budget selected".  The reader leaves `clock` None only for a file torn
/// before its CLOCKS chunk (a zero frequency is MALFORMED), and such a file has no FRAME or extension, so the state is
/// built by clearing the clock of a parsed capture: the page must still say "no clock" if it ever arises.
#[test]
fn no_clock_draw_path_says_no_clock() {
    use eframe::egui;
    use ps3dk_prof_viewer::frames_ui::{draw, FramesState};
    let mut a = analysed(&owner_bytes(&[0, 150, 300], false, cm_ext()));
    a.capture.clock = None;
    let ix = FramesIndex::build(&a);
    assert_eq!(ix.freq_hz, None);
    let slot = ForeignSlot::default();
    let mut st = FramesState::default();
    st.reset(Some(&ix));
    let ctx = egui::Context::default();
    ps3dk_ui::install(&ctx);
    for _ in 0..3 {
        let _ = ctx.run(egui::RawInput::default(), |ctx| {
            egui::CentralPanel::default().show(ctx, |p| {
                draw(p, &a, &ix, &mut st, None, Some(&slot));
            });
        });
    }
    assert_eq!(slot.counters.get(), (1, 1), "three repaints: one reader pass, one view");
    // the page's own view (the cache hit proves it is the one the draw path built)
    let v = slot.view(&a, &ix, 1, false, Budget::None);
    assert_eq!(slot.counters.get(), (1, 1));
    let rsx = v.lanes.iter().find(|l| l.label == "RSX domain 3").unwrap();
    let b = rsx.cells[&1].bar.as_ref().unwrap();
    assert_eq!(b.verdict, VerdictView::NoClock);
    assert_eq!(b.verdict.text(), "verdict unavailable (no clock)");
    assert!(b.label.contains(" ticks") && !b.label.contains(" ms"), "ticks only without a clock: {}", b.label);
    // with a usable clock, an explicit "No budget" stays NoBudget
    let c = analysed(&owner_bytes(&[0, 150, 300], false, cm_ext()));
    let cix = FramesIndex::build(&c);
    let cv = ForeignSlot::default().view(&c, &cix, 1, false, Budget::None);
    let cb = cv.lanes.iter().find(|l| l.label == "RSX domain 3").unwrap().cells[&1].bar.clone().unwrap();
    assert_eq!(cb.verdict, VerdictView::NoBudget);
}

/// Excluded-only and candidate-only cells keep their counts and draw no bar (NonePlaced).
#[test]
fn excluded_only_and_candidate_only_cells_keep_counts_without_a_bar() {
    let a = analysed(&f2_bytes(cm_ext()));
    let ix = FramesIndex::build(&a);
    let o = ix.owner(1).unwrap();
    let mut excluded = cell(0, None);
    excluded.excluded_extrapolated = 1;
    excluded.excluded_exclusive_unavailable = 1;
    let mut candidate = cell(1, None);
    candidate.may_belong = 2;
    candidate.may_belong_beyond = 1;
    let v = build(&a, &ix, 1, &Ok(handcrafted(vec![excluded, candidate])), &Labels::build(&a), Budget::Fps30);
    let lane = &v.lanes[0];
    let (e, c) = (&lane.cells[&0], &lane.cells[&1]);
    assert!(e.bar.is_none() && e.bar_exact.is_none() && e.rows.is_empty());
    assert!(e.counts.contains("extrapolated 1, exclusive unavailable 1"), "{}", e.counts);
    assert!(c.bar.is_none() && c.counts.contains("may belong here 2 (beyond the measured frames 1"), "{}", c.counts);
    assert_eq!(mark(o, lane, 0), Mark::NonePlaced);
    assert_eq!(mark(o, lane, 1), Mark::NonePlaced);
}

fn empty_lane() -> ps3dk_prof_viewer::foreign::LaneView {
    ps3dk_prof_viewer::foreign::LaneView {
        label: "RSX domain 3".to_string(),
        epochs: Vec::new(),
        cells: BTreeMap::new(),
        footer: Vec::new(),
        errors: Vec::new(),
    }
}

/// UNKNOWN (final), REFUSED (owner NOT VALID) and zero-length measured slots are NoInterval; a positive measured
/// slot with nothing included is NonePlaced.
#[test]
fn no_interval_for_unknown_refused_and_zero_length_slots() {
    use ps3dk_prof_viewer::frames::SlotKind;
    let l = empty_lane();
    // frames 0 [0, 150), 1 [150, 150) zero-length, 2 [150, 300), 3 final
    let a = analysed(&owner_bytes(&[0, 150, 150, 300], false, cm_ext()));
    let ix = FramesIndex::build(&a);
    let o = ix.owner(1).unwrap();
    assert_eq!(o.slots[1].kind, SlotKind::Measured(0));
    assert!(matches!(o.slots[3].kind, SlotKind::Unknown(_)));
    assert_eq!(mark(o, &l, 0), Mark::NonePlaced);
    assert_eq!(mark(o, &l, 1), Mark::NoInterval, "zero-length");
    assert_eq!(mark(o, &l, 2), Mark::NonePlaced);
    assert_eq!(mark(o, &l, 3), Mark::NoInterval, "UNKNOWN (final)");
    let b = analysed(&owner_bytes(&[0, 150, 300], true, cm_ext()));
    let bix = FramesIndex::build(&b);
    let bo = bix.owner(1).unwrap();
    assert!(bo.slots.iter().all(|s| matches!(s.kind, SlotKind::Refused(_))), "{:?}", bo.slots);
    for i in 0..bo.slots.len() {
        assert_eq!(mark(bo, &l, i), Mark::NoInterval, "REFUSED slot {i}");
    }
}

// ---- F3b: SPU/RSX lanes on the Hot paths page ------------------------------------------------------------------

#[test]
fn hot_paths_foreign_lanes_w32_and_cache() {
    use ps3dk_prof_viewer::foreign_hot::ForeignHotSlot;
    use std::sync::atomic::Ordering;
    let a = analysed(&f2_bytes(cm_ext()));
    let slot = ForeignHotSlot::default();
    let off = slot.view(&a, false);
    let spu = off.lanes.iter().find(|l| l.label == "SPU lane 10").expect("SPU lane");
    assert!(spu.coverage.iter().any(|c| c.contains("AMBIGUOUS-LIFT") && c.contains("coverage unknown")), "{spu:?}");
    assert!(spu.rows.is_empty());
    let rsx = off.lanes.iter().find(|l| l.label == "RSX domain 3").expect("RSX lane");
    assert_eq!(rsx.rows.len(), 1);
    assert_eq!(rsx.rows[0].leaf, "desc 3 gpuzone");
    assert!(rsx.rows[0].exclusive_exact.starts_with("[290/3 ticks, 310/3 ticks]"), "{}", rsx.rows[0].exclusive_exact);
    // one row: [lo / sum hi, hi / sum lo] = [290/310 rounded down, 310/290 clamped to 1]
    assert!(rsx.rows[0].share.starts_with("share [93.5%, 100.0%]"), "{}", rsx.rows[0].share);
    let on = slot.view(&a, true);
    let spu = on.lanes.iter().find(|l| l.label == "SPU lane 10").unwrap();
    assert_eq!(spu.rows.len(), 1);
    assert_eq!(spu.rows[0].leaf, "image 0 desc 0 spuzone0");
    assert!(!spu.rows[0].partial);
    for _ in 0..10 {
        drop(slot.view(&a, true));
    }
    assert_eq!(slot.builds.load(Ordering::Relaxed), 2, "one build per W32 value; repaints reuse it");
}

#[test]
fn hot_paths_foreign_counts_unknown_pairs_population_and_exact_inclusive() {
    use ps3dk_prof_viewer::foreign_hot::ForeignHotSlot;
    let a = analysed(&f2_bytes(cm_ext()));
    let slot = ForeignHotSlot::default();
    let off = slot.view(&a, false);
    let spu = off.lanes.iter().find(|l| l.label == "SPU lane 10").unwrap();
    assert!(spu.coverage.iter().all(|c| c.ends_with("pairs unknown")), "no inventory: unknown, never 0: {spu:?}");
    let rsx = off.lanes.iter().find(|l| l.label == "RSX domain 3").unwrap();
    assert!(rsx.lines[0].starts_with("pairs in summed epochs (population): 1 = included 1"), "{:?}", rsx.lines);
    assert!(rsx.lines[1].starts_with("kept in paths: 1"), "{:?}", rsx.lines);
    assert!(rsx.lines[3].starts_with("non-pair events in summed epochs:"), "{:?}", rsx.lines);
    assert!(rsx.rows[0].inclusive_exact.starts_with('['), "{}", rsx.rows[0].inclusive_exact);
}

#[test]
fn hot_paths_a_premise_change_clears_the_selected_row_before_the_view_is_taken() {
    use ps3dk_prof_viewer::hotpaths_ui::{apply_premise, HotState};
    let mut st = HotState { row: Some(2), ..HotState::default() };
    apply_premise(&mut st, false);
    assert_eq!((st.w32, st.row), (false, Some(2)), "no change keeps the row");
    apply_premise(&mut st, true);
    assert_eq!((st.w32, st.row), (true, None), "a change clears it");
}

#[test]
fn hot_paths_foreign_partial_root_is_labelled() {
    use ps3dk_prof_viewer::foreign_hot::ForeignHotSlot;
    // A (desc 0) BEGIN at depth 0 stays open; B (desc 1) completes at depth 1: a PARTIAL(missing 1) root
    let mut ext = cm_ext();
    ext[6] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (980, 1, 2, 1)]);
    ext[8] = finals(&[(LANE, 1, 1, 3, 3, 0, 1), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let a = analysed(&f2_bytes(ext));
    let v = ForeignHotSlot::default().view(&a, true);
    let spu = v.lanes.iter().find(|l| l.label == "SPU lane 10").unwrap();
    assert_eq!(spu.rows.len(), 1, "{spu:?}");
    assert!(spu.rows[0].partial);
    assert!(spu.rows[0].text.starts_with("PARTIAL(missing 1) > image 0 desc 1 spuzone1"), "{}", spu.rows[0].text);
}

// ---- Timeline SPU/RSX rows (director 2026-11-04 19:32) ----------------------------------------------------------

mod tl_rows {
    use super::*;
    use ps3dk_prof::clockmap::{End, Projection};
    use ps3dk_prof::foreign_timeline::{FtBar, FtCounts, FtEpoch, FtEpochRow, FtSource, FtSourceState};
    use ps3dk_prof_viewer::model::Document;
    use ps3dk_prof_viewer::timeline::{timeline, View};
    use ps3dk_prof_viewer::timeline_foreign::{build_track, hit, layout, FDrawKind, FTrack, QueryCaps, WIDE_TILES};
    use ps3dk_prof_viewer::timeline_ui::TimelineState;
    use std::path::PathBuf;

    fn at(n: i64, d: i64, open: bool) -> End {
        End::At { value: rat(n, d), open }
    }

    fn fbar(b: (End, End), e: (End, End), depth: u8, extrapolated: bool) -> FtBar {
        FtBar {
            epoch: 1,
            image: Some(0),
            desc: 0,
            depth,
            begin: Projection { lo: b.0, hi: b.1, interpolated: true },
            end: Projection { lo: e.0, hi: e.1, interpolated: true },
            inclusive: 5,
            exclusive: None,
            extrapolated,
        }
    }

    fn src(bars: Vec<FtBar>) -> FtSource {
        FtSource {
            source: SourceKey::Lane(LANE),
            epochs: vec![FtEpochRow { epoch: 1, state: FtEpoch::Placed { bars: bars.len() as u64, too_deep: 0 }, pairs: Some(bars.len() as u64) }],
            counts: FtCounts { placed: bars.len() as u64, open: Some(0), loss_records: Some(0), ..FtCounts::default() },
            state: FtSourceState::Bars(bars),
        }
    }

    fn view(t0: f64, dt: f64) -> View {
        View { t0, dt }
    }

    fn one(t: &FTrack, v: &View) -> ps3dk_prof_viewer::timeline_foreign::FDraw {
        let l = layout(t, v, 100.0, QueryCaps::default(), true);
        assert_eq!(l.draws.len(), 1, "{l:?}");
        l.draws[0]
    }

    #[test]
    fn core_rounds_inward_and_whiskers_outward() {
        // begin [10.2, 10.4], end [20.6, 20.8] ticks; 1 px per tick
        let t = build_track(&src(vec![fbar((at(51, 5, false), at(52, 5, false)), (at(103, 5, false), at(104, 5, false)), 0, false)]), 0);
        let d = one(&t, &view(0.0, 100.0));
        assert_eq!((d.w0, d.w1), (10.0, 21.0));
        assert_eq!(d.core, Some((11.0, 20.0)));
        assert!(!d.arrow_left && !d.arrow_right);
    }

    #[test]
    fn touching_bounds_and_sub_pixel_cores_draw_no_core() {
        let touch = build_track(&src(vec![fbar((at(10, 1, false), at(20, 1, false)), (at(20, 1, false), at(30, 1, false)), 0, false)]), 0);
        assert_eq!(one(&touch, &view(0.0, 100.0)).core, None, "exact touch: no core");
        // a real core of 0.5 tick: inward rounding leaves under a pixel -> none
        let thin = build_track(&src(vec![fbar((at(10, 1, false), at(102, 10, false)), (at(107, 10, false), at(30, 1, false)), 0, false)]), 0);
        assert!(thin.bars[0].core, "exactly a core");
        assert_eq!(one(&thin, &view(0.0, 100.0)).core, None, "under a pixel after inward rounding");
        // point bounds: no fabricated width
        let point = build_track(&src(vec![fbar((at(10, 1, false), at(10, 1, false)), (at(10, 1, false), at(10, 1, false)), 0, false)]), 0);
        let l = layout(&point, &view(0.0, 100.0), 100.0, QueryCaps::default(), true);
        assert_eq!(l.draws.len(), 1);
        assert!(matches!(l.draws[0].kind, FDrawKind::Bar(0)) && l.draws[0].tick && l.draws[0].core.is_none(), "{l:?}");
        assert_eq!(hit(&l, 0, 10.0), Some(0), "a lone point is a tick with its own hover");
    }

    #[test]
    fn unbounded_sides_reach_the_edge_with_arrows_and_open_finite_ends_do_not() {
        let t = build_track(&src(vec![fbar((End::NegInf, at(20, 1, false)), (at(30, 1, false), End::PosInf), 0, false)]), 0);
        let d = one(&t, &view(0.0, 100.0));
        assert_eq!((d.w0, d.w1, d.arrow_left, d.arrow_right), (0.0, 100.0, true, true));
        assert_eq!(d.core, Some((20.0, 30.0)), "outer unbounded sides keep the finite core");
        let open = build_track(&src(vec![fbar((at(10, 1, true), at(20, 1, false)), (at(30, 1, false), at(40, 1, true)), 0, false)]), 0);
        let d = one(&open, &view(0.0, 100.0));
        assert!(!d.arrow_left && !d.arrow_right, "open finite is finite: {d:?}");
        assert_eq!((d.w0, d.w1), (10.0, 40.0));
    }

    #[test]
    fn huge_absolute_ticks_with_one_tick_deltas_keep_their_geometry() {
        let o: i64 = 1 << 62;
        let t = build_track(&src(vec![fbar((at(o + 10, 1, false), at(o + 11, 1, false)), (at(o + 13, 1, false), at(o + 14, 1, false)), 0, false)]), o as u64);
        let d = one(&t, &view(0.0, 100.0));
        assert_eq!((d.w0, d.w1, d.core), (10.0, 14.0, Some((11.0, 13.0))));
    }

    #[test]
    fn chart_range_clips_cores_and_whiskers_never_extends_them() {
        let t = build_track(&src(vec![fbar((at(-50, 1, false), at(-40, 1, false)), (at(150, 1, false), at(160, 1, false)), 0, false)]), 0);
        let d = one(&t, &view(0.0, 100.0));
        assert_eq!((d.w0, d.w1, d.core), (0.0, 100.0, Some((0.0, 100.0))));
    }

    #[test]
    fn a_bar_spanning_tiles_is_drawn_once_and_wide_bars_are_listed_once() {
        let mut bars: Vec<FtBar> = (0..50).map(|i| fbar((at(i * 10, 1, false), at(i * 10 + 1, 1, false)), (at(i * 10 + 3, 1, false), at(i * 10 + 4, 1, false)), 0, false)).collect();
        // spans a few tiles (under WIDE_TILES) and one spanning everything (wide)
        bars.push(fbar((at(5, 1, false), at(6, 1, false)), (at(9, 1, false), at(9 + (WIDE_TILES as i64 / 2), 1, false)), 1, false));
        bars.push(fbar((at(0, 1, false), at(1, 1, false)), (at(499, 1, false), at(504, 1, false)), 2, false));
        let n = bars.len();
        let t = build_track(&src(bars), 0);
        let l = layout(&t, &view(0.0, 510.0), 1020.0, QueryCaps::default(), true);
        let mut seen: Vec<usize> = l.draws.iter().filter_map(|d| match d.kind { FDrawKind::Bar(i) => Some(i), _ => None }).collect();
        let dense: usize = l.draws.iter().map(|d| match d.kind { FDrawKind::Dense { count } => count, _ => 0 }).sum();
        seen.sort();
        let before = seen.len();
        seen.dedup();
        assert_eq!(before, seen.len(), "each bar drawn once");
        assert_eq!(seen.len() + dense, n, "every bar in view drawn or merged: {l:?}");
        assert_eq!(l.omitted, None);
    }

    #[test]
    fn paint_and_inspection_caps_say_what_was_omitted() {
        let bars: Vec<FtBar> = (0..20).map(|i| fbar((at(i * 5, 1, false), at(i * 5 + 1, 1, false)), (at(i * 5 + 3, 1, false), at(i * 5 + 4, 1, false)), 0, false)).collect();
        let t = build_track(&src(bars), 0);
        let v = view(0.0, 100.0);
        let paint = layout(&t, &v, 1000.0, QueryCaps { inspect_cap: 1000, paint_cap: 5 }, true);
        assert_eq!(paint.omitted, Some(Some(15)), "exact: every candidate was inspected");
        assert_eq!(paint.draws.len(), 5);
        let insp = layout(&t, &v, 1000.0, QueryCaps { inspect_cap: 3, paint_cap: 1000 }, true);
        assert_eq!(insp.omitted, Some(None), "inspection stopped: count unknown");
        assert_eq!(insp.inspected, 3);
    }

    #[test]
    fn an_all_wide_track_is_bounded_by_the_inspection_cap() {
        let bars: Vec<FtBar> = (0..30).map(|i| fbar((End::NegInf, at(i, 1, false)), (at(i + 2, 1, false), End::PosInf), 0, false)).collect();
        let t = build_track(&src(bars), 0);
        let full = layout(&t, &view(0.0, 100.0), 100.0, QueryCaps::default(), true);
        assert_eq!(full.draws.len(), 30);
        let capped = layout(&t, &view(0.0, 100.0), 100.0, QueryCaps { inspect_cap: 7, paint_cap: 1000 }, true);
        assert_eq!((capped.inspected, capped.draws.len(), capped.omitted), (7, 7, Some(None)));
    }

    #[test]
    fn hidden_extrapolated_bars_are_not_drawn_and_not_omitted() {
        let t = build_track(&src(vec![fbar((at(10, 1, false), at(11, 1, false)), (at(20, 1, false), at(21, 1, false)), 0, true)]), 0);
        assert!(one(&t, &view(0.0, 100.0)).extrapolated);
        let hid = layout(&t, &view(0.0, 100.0), 100.0, QueryCaps::default(), false);
        assert!(hid.draws.is_empty() && hid.omitted.is_none());
    }

    #[test]
    fn hover_identity_under_overlap_is_the_last_drawn_bar() {
        let t = build_track(&src(vec![
            fbar((at(10, 1, false), at(11, 1, false)), (at(50, 1, false), at(51, 1, false)), 0, false),
            fbar((at(20, 1, false), at(21, 1, false)), (at(30, 1, false), at(31, 1, false)), 0, false),
            fbar((at(20, 1, false), at(21, 1, false)), (at(30, 1, false), at(31, 1, false)), 1, false),
        ]), 0);
        let l = layout(&t, &view(0.0, 100.0), 100.0, QueryCaps::default(), true);
        assert_eq!(hit(&l, 0, 25.0), Some(1), "the later bar at depth 0");
        assert_eq!(hit(&l, 0, 45.0), Some(0));
        assert_eq!(hit(&l, 1, 25.0), Some(2), "depth decides the row");
        assert_eq!(hit(&l, 0, 25.0), hit(&l, 0, 25.0), "stable");
    }

    #[test]
    fn rows_are_cached_per_document_and_w32_and_spu_comes_before_rsx() {
        let bytes = f2_bytes(cm_ext());
        let doc = Document::from_bytes(PathBuf::from("t.prof"), &bytes);
        let a = doc.analysis().unwrap();
        let tl = timeline(a);
        let r64 = doc.foreign_tl.view(a, &tl, false);
        let again = doc.foreign_tl.view(a, &tl, false);
        assert!(std::sync::Arc::ptr_eq(&r64, &again));
        let w32 = doc.foreign_tl.view(a, &tl, true);
        assert_eq!(doc.foreign_tl.builds.load(std::sync::atomic::Ordering::Relaxed), 2);
        let labels: Vec<&str> = w32.tracks.iter().map(|t| t.label.as_str()).collect();
        assert_eq!(labels, vec!["SPU lane 10", "RSX domain 3"]);
        // R64: the SPU lane is an ambiguous lift -> coverage text, no bars; RSX unaffected
        let spu = &r64.tracks[0];
        assert!(spu.bars.is_empty() && spu.coverage.iter().any(|c| c.contains("ambiguous")), "{spu:?}");
        assert!(!r64.tracks[1].bars.is_empty());
        assert!(!w32.tracks[0].bars.is_empty());
        assert!(r64.caption.contains("R64") && w32.caption.contains("W32"));
        assert!(spu.header.contains("1 epoch(s) without an inventory: their events are unknown"), "{}", spu.header);
        let rsx = &r64.tracks[1];
        assert!(rsx.header.contains("events in 1 epoch(s) with an inventory") && rsx.header.contains("(pairs unknown)"), "{}", rsx.header);
    }

    #[test]
    fn a_w32_toggle_never_draws_old_premise_rows_beside_the_new_caption() {
        let bytes = f2_bytes(cm_ext());
        let doc = Document::from_bytes(PathBuf::from("t.prof"), &bytes);
        let a = doc.analysis().unwrap();
        let mut st = TimelineState::new(a);
        st.sync_foreign(&doc.foreign_tl, a);
        assert!(!st.foreign.as_ref().unwrap().w32);
        st.w32 = true;
        st.sync_foreign(&doc.foreign_tl, a); // what draw() does first, every frame
        let f = st.foreign.as_ref().unwrap();
        assert!(f.w32 && f.caption.contains("W32") && !f.tracks[0].bars.is_empty());
    }

    #[test]
    fn a_foreign_only_capture_takes_its_axis_from_interpolated_bounds() {
        let mut s = scenario(vec![lane(1, Vec::new())]); // declared, no records: no PPU data
        let mut gd = desc(GPU_DESC, "gpuzone", "");
        gd.scope = 2;
        s.descs.push(gd);
        let mut f = s.file();
        let at_ = f.find(T_TRAILER);
        for (i, c) in cm_ext().into_iter().enumerate() {
            f.chunks.insert(at_ + i, c);
        }
        f.flags = 1;
        let bytes = f.bytes();
        let doc = Document::from_bytes(PathBuf::from("t.prof"), &bytes);
        let a = doc.analysis().unwrap();
        let mut st = TimelineState::new(a);
        assert!(!st.tl.has_data);
        st.w32 = true;
        st.sync_foreign(&doc.foreign_tl, a);
        let fv = st.foreign.clone().unwrap();
        let (lo, hi) = fv.extent.expect("finite interpolated bounds exist");
        assert_eq!((st.tl.start, st.tl.end), (lo, hi));
        assert!(hi > lo);
        assert_eq!(fv.origin, lo);
        // an empty capture: no extension, no axis, no rows
        let empty = scenario(vec![]).bytes();
        let d2 = Document::from_bytes(PathBuf::from("e.prof"), &empty);
        let a2 = d2.analysis().unwrap();
        let mut st2 = TimelineState::new(a2);
        st2.sync_foreign(&d2.foreign_tl, a2);
        assert!(st2.foreign.as_ref().unwrap().tracks.is_empty());
        assert_eq!((st2.tl.start, st2.tl.end), (0, 0));
    }
}

#[test]
fn tl_rows_sub_pixel_bars_merge_into_dense_runs_and_runs_count_against_the_paint_cap() {
    use ps3dk_prof::clockmap::{End, Projection};
    use ps3dk_prof::foreign_timeline::{FtBar, FtCounts, FtSource, FtSourceState};
    use ps3dk_prof_viewer::timeline::View;
    use ps3dk_prof_viewer::timeline_foreign::{build_track, layout, FDrawKind, QueryCaps};
    let at = |v: i64| End::At { value: rat(v, 1), open: false };
    let bars: Vec<FtBar> = (0..1000)
        .map(|i| FtBar {
            epoch: 1,
            image: Some(0),
            desc: 0,
            depth: 0,
            begin: Projection { lo: at(i * 100), hi: at(i * 100 + 1), interpolated: true },
            end: Projection { lo: at(i * 100 + 3), hi: at(i * 100 + 4), interpolated: true },
            inclusive: 3,
            exclusive: Some(3),
            extrapolated: false,
        })
        .collect();
    let s = FtSource { source: SourceKey::Lane(LANE), epochs: vec![], counts: FtCounts::default(), state: FtSourceState::Bars(bars) };
    let t = build_track(&s, 0);
    let l = layout(&t, &View { t0: 0.0, dt: 100_000.0 }, 100.0, QueryCaps { inspect_cap: 10_000, paint_cap: 10_000 }, true);
    let dense: usize = l.draws.iter().map(|d| match d.kind { FDrawKind::Dense { count } => count, _ => 0 }).sum();
    assert_eq!(dense, 1000, "{l:?}");
    assert!(l.draws.len() < 200 && l.omitted.is_none(), "{} draws, {:?}", l.draws.len(), l.omitted);
    // one primitive allowed: one run is drawn, every other bar is reported exactly as not drawn
    let one = layout(&t, &View { t0: 0.0, dt: 100_000.0 }, 100.0, QueryCaps { inspect_cap: 10_000, paint_cap: 1 }, true);
    let drawn: usize = one.draws.iter().map(|d| match d.kind { FDrawKind::Dense { count } => count, _ => 1 }).sum();
    assert_eq!(one.draws.len(), 1);
    let want = if drawn == 1000 { None } else { Some(Some(1000 - drawn)) };
    assert_eq!(one.omitted, want, "{one:?}");
}

#[test]
fn tl_rows_bars_outside_the_axis_are_counted_in_the_header() {
    use ps3dk_prof::clockmap::{End, Projection};
    use ps3dk_prof::foreign_timeline::{FtBar, FtCounts, FtSource, FtSourceState, ForeignTimeline};
    use ps3dk_prof_viewer::timeline_foreign::build;
    let at = |v: i64| End::At { value: rat(v, 1), open: false };
    let b = |lo: i64, hi: i64| FtBar {
        epoch: 1,
        image: Some(0),
        desc: 0,
        depth: 0,
        begin: Projection { lo: at(lo), hi: at(lo + 1), interpolated: true },
        end: Projection { lo: at(hi - 1), hi: at(hi), interpolated: true },
        inclusive: 1,
        exclusive: Some(1),
        extrapolated: false,
    };
    let ft = ForeignTimeline {
        sources: vec![FtSource {
            source: SourceKey::Lane(LANE),
            epochs: vec![],
            counts: FtCounts::default(),
            state: FtSourceState::Bars(vec![b(100, 200), b(1000, 1100), b(1500, 1600), b(2100, 2200), b(2300, 2400)]),
        }],
        ..ForeignTimeline::default()
    };
    let a = analysed(&f2_bytes(cm_ext()));
    let v = build(&a, &ft, 1000, 1000, true);
    assert!(v.tracks[0].header.contains("outside the PPU data range: 1 before, 2 after"), "{}", v.tracks[0].header);
    let inside = build(&a, &ft, 0, 10_000, true);
    assert!(!inside.tracks[0].header.contains("outside"), "{}", inside.tracks[0].header);
}

mod tl_rows_r2 {
    use super::*;
    use ps3dk_prof::clockmap::{End, Projection};
    use ps3dk_prof::foreign_timeline::{FtBar, FtCounts, FtSource, FtSourceState};
    use ps3dk_prof_viewer::model::Document;
    use ps3dk_prof_viewer::timeline::View;
    use ps3dk_prof_viewer::timeline_foreign::{
        build_track, build_track_with, hit, hit_dense, layout, FDrawKind, QueryCaps, ViewBudgets, TILES, WIDE_TILES,
    };
    use ps3dk_prof_viewer::timeline_ui::TimelineState;
    use std::path::PathBuf;

    fn at(v: i64) -> End {
        End::At { value: rat(v, 1), open: false }
    }

    fn b(lo: i64, hi: i64, extrapolated: bool) -> FtBar {
        FtBar {
            epoch: 1,
            image: Some(0),
            desc: 0,
            depth: 0,
            begin: Projection { lo: at(lo), hi: at(lo), interpolated: !extrapolated },
            end: Projection { lo: at(hi), hi: at(hi), interpolated: !extrapolated },
            inclusive: 1,
            exclusive: Some(1),
            extrapolated,
        }
    }

    fn src(bars: Vec<FtBar>) -> FtSource {
        FtSource { source: SourceKey::Lane(LANE), epochs: vec![], counts: FtCounts::default(), state: FtSourceState::Bars(bars) }
    }

    #[test]
    fn a_zero_paint_cap_draws_nothing_and_reports_every_bar() {
        let t = build_track(&src((0..10).map(|i| b(i * 10, i * 10 + 5, false)).collect()), 0);
        let l = layout(&t, &View { t0: 0.0, dt: 100.0 }, 1000.0, QueryCaps { inspect_cap: 1000, paint_cap: 0 }, true);
        assert!(l.draws.is_empty());
        assert_eq!(l.omitted, Some(Some(10)));
        // sub-pixel runs too
        let l = layout(&t, &View { t0: 0.0, dt: 100_000.0 }, 10.0, QueryCaps { inspect_cap: 1000, paint_cap: 0 }, true);
        assert!(l.draws.is_empty());
        assert_eq!(l.omitted, Some(Some(10)));
    }

    #[test]
    fn separated_dense_runs_are_each_one_primitive() {
        // three clusters of three sub-pixel bars, far apart: three runs
        let mut v = Vec::new();
        for c in [0i64, 40_000, 80_000] {
            for k in 0..3 {
                v.push(b(c + k * 2, c + k * 2 + 1, false));
            }
        }
        let t = build_track(&src(v), 0);
        let view = View { t0: 0.0, dt: 100_000.0 };
        let l = layout(&t, &view, 100.0, QueryCaps::default(), true);
        assert_eq!(l.draws.len(), 3, "{l:?}");
        assert!(l.draws.iter().all(|d| matches!(d.kind, FDrawKind::Dense { count: 3 })));
        let capped = layout(&t, &view, 100.0, QueryCaps { inspect_cap: 1000, paint_cap: 2 }, true);
        assert_eq!((capped.draws.len(), capped.omitted), (2, Some(Some(3))));
    }

    #[test]
    fn extrapolated_and_interpolated_sub_pixel_bars_never_share_a_run() {
        let t = build_track(&src(vec![b(0, 1, false), b(2, 3, true), b(4, 5, false), b(6, 7, true)]), 0);
        let l = layout(&t, &View { t0: 0.0, dt: 100_000.0 }, 100.0, QueryCaps::default(), true);
        assert_eq!(l.draws.len(), 2, "{l:?}");
        let mut styles: Vec<(bool, usize)> = l
            .draws
            .iter()
            .map(|d| (d.extrapolated, match d.kind { FDrawKind::Dense { count } => count, _ => 1 }))
            .collect();
        styles.sort();
        assert_eq!(styles, vec![(false, 2), (true, 2)]);
        let x = l.draws.iter().find(|d| d.extrapolated).unwrap();
        assert_eq!(hit_dense(&l, 0, x.w0), Some((2, true)));
        assert!(ps3dk_prof_viewer::timeline_foreign::dense_text(2, true).contains("EXTRAPOLATED"));
        // a lone extrapolated point keeps its style as a tick
        let one = build_track(&src(vec![b(10, 10, true)]), 0);
        let l = layout(&one, &View { t0: 0.0, dt: 100.0 }, 100.0, QueryCaps::default(), true);
        assert!(l.draws[0].tick && l.draws[0].extrapolated, "{l:?}");
        assert_eq!(hit(&l, 0, 10.0), Some(0));
    }

    #[test]
    fn duplicate_tile_entries_count_as_inspections() {
        // span = [0, 1024): 1 tick per tile; the bar [100, 103] sits in 4 tiles (under WIDE_TILES)
        let t = build_track(&src(vec![b(0, 1, false), b(100, 103, false), b(1022, 1023, false)]), 0);
        let l = layout(&t, &View { t0: 0.0, dt: 1023.0 }, 2000.0, QueryCaps::default(), true);
        assert_eq!(l.inspected, 2 + 4 + 2, "every entry, duplicates included");
        let ids: Vec<usize> = l.draws.iter().filter_map(|d| match d.kind { FDrawKind::Bar(i) => Some(i), _ => None }).collect();
        assert_eq!(ids, vec![0, 1, 2], "each bar drawn once");
        const { assert!(4 <= WIDE_TILES) };
    }

    #[test]
    fn the_view_build_budget_is_exact_and_one_short_refuses_the_drawing() {
        let bars: Vec<FtBar> = (0..5).map(|i| b(i * 100, i * 100 + 50, false)).collect();
        let s = src(bars);
        let full = build_track(&s, 0);
        // independent count: 6 per bar, the gap pass (1 per bar + GAP_MAX), the tile table, one entry per covered
        // tile (no wide bars here)
        let lo = 0.0;
        let hi = 450.0;
        let tile = |v: f64| (((v - lo) / (hi - lo) * TILES as f64).floor() as usize).min(TILES - 1);
        let entries: u64 = (0..5).map(|i| (tile((i * 100 + 50) as f64) - tile((i * 100) as f64) + 1).min(WIDE_TILES + 1) as u64).map(|e| if e > WIDE_TILES as u64 { 1 } else { e }).sum();
        let need = 6 * 5 + (5 + ps3dk_prof_viewer::timeline_foreign::GAP_MAX as u64) + TILES as u64 + entries;
        let exact = build_track_with(&s, 0, ViewBudgets { build_ops: need });
        assert!(exact.refusal.is_none() && exact.bars.len() == 5, "{:?}", exact.refusal);
        assert_eq!(exact.gaps, vec![(50.0, 100.0), (150.0, 200.0), (250.0, 300.0), (350.0, 400.0)]);
        assert_eq!(exact, full);
        let short = build_track_with(&s, 0, ViewBudgets { build_ops: need - 1 });
        assert!(short.refusal.as_deref().unwrap_or("").contains("view build budget") && short.bars.is_empty());
        let l = layout(&short, &View { t0: 0.0, dt: 500.0 }, 100.0, QueryCaps::default(), true);
        assert!(l.draws.is_empty() && l.inspected == 0);
    }

    fn rb(lo: (i64, i64), hi: (i64, i64)) -> FtBar {
        let e = |(n, d): (i64, i64)| End::At { value: rat(n, d), open: false };
        FtBar {
            epoch: 1,
            image: Some(0),
            desc: 0,
            depth: 0,
            begin: Projection { lo: e(lo), hi: e(lo), interpolated: true },
            end: Projection { lo: e(hi), hi: e(hi), interpolated: true },
            inclusive: 1,
            exclusive: Some(1),
            extrapolated: false,
        }
    }

    #[test]
    fn a_wide_bar_visited_after_tiled_ones_never_joins_a_run_on_its_far_side() {
        // review 20:52: outer [0,10] (wide: 11 tiles), [900, 900.1] and [1000, 1000] (tiled); visit order 1, 2, 0
        let t = build_track(&src(vec![rb((0, 1), (10, 1)), rb((900, 1), (9001, 10)), rb((1000, 1), (1000, 1))]), 0);
        let l = layout(&t, &View { t0: 0.0, dt: 2000.0 }, 100.0, QueryCaps::default(), true);
        let mut ids: Vec<usize> = l.draws.iter().filter_map(|d| match d.kind { FDrawKind::Bar(i) => Some(i), _ => None }).collect();
        ids.sort();
        assert_eq!(ids, vec![0, 1, 2], "every bar drawn as its own tick: {l:?}");
        assert_eq!(l.omitted, None);
        let b0 = l.draws.iter().find(|d| d.kind == FDrawKind::Bar(0)).unwrap();
        assert!(b0.w0 <= 0.5 && b0.tick, "{b0:?}");
        // an overlapping visit in reverse order grows the run both ways
        let t = build_track(&src(vec![rb((0, 1), (10, 1)), rb((5, 1), (6, 1)), rb((8, 1), (9, 1))]), 0);
        let l = layout(&t, &View { t0: 0.0, dt: 2000.0 }, 100.0, QueryCaps::default(), true);
        assert_eq!(l.draws.len(), 1, "{l:?}");
        assert!(matches!(l.draws[0].kind, FDrawKind::Dense { count: 3 }) && l.draws[0].w0 <= 0.0, "{l:?}");
    }

    #[test]
    fn hover_resolves_bars_and_runs_by_paint_order_in_both_stackings() {
        use ps3dk_prof_viewer::timeline_foreign::{hit_any, FDraw, FHit, FLayout};
        let bar = FDraw { depth: 0, w0: 10.0, w1: 20.0, core: None, arrow_left: false, arrow_right: false, extrapolated: false, tick: false, kind: FDrawKind::Bar(7) };
        let run = FDraw { depth: 0, w0: 12.0, w1: 14.0, core: None, arrow_left: false, arrow_right: false, extrapolated: true, tick: true, kind: FDrawKind::Dense { count: 4 } };
        let above = FLayout { draws: vec![bar, run], inspected: 0, omitted: None };
        assert_eq!(hit_any(&above, 0, 13.0), Some(FHit::Dense { count: 4, extrapolated: true }), "the run is drawn on top");
        assert_eq!(hit(&above, 0, 13.0), None, "the bar below never steals the hover");
        assert_eq!(hit_any(&above, 0, 18.0), Some(FHit::Bar(7)));
        let below = FLayout { draws: vec![run, bar], inspected: 0, omitted: None };
        assert_eq!(hit_any(&below, 0, 13.0), Some(FHit::Bar(7)), "the bar is drawn on top");
        assert_eq!(hit_dense(&below, 0, 13.0), None);
        let plain = FDraw { extrapolated: false, ..run };
        let mixed = FLayout { draws: vec![run, plain], inspected: 0, omitted: None };
        assert_eq!(hit_any(&mixed, 0, 13.0), Some(FHit::Dense { count: 4, extrapolated: false }), "the topmost style answers");
    }

    #[test]
    fn the_outside_count_promises_fit_all_only_for_interpolated_bars() {
        use ps3dk_prof::foreign_timeline::ForeignTimeline;
        use ps3dk_prof_viewer::timeline_foreign::build;
        let mut x = b(-500, -400, true);
        x.begin.interpolated = false;
        let ft = ForeignTimeline {
            sources: vec![FtSource { source: SourceKey::Lane(LANE), epochs: vec![], counts: FtCounts::default(), state: FtSourceState::Bars(vec![b(-300, -200, false), x, b(50, 60, false)]) }],
            ..ForeignTimeline::default()
        };
        let a = analysed(&f2_bytes(cm_ext()));
        let v = build(&a, &ft, 0, 100, true);
        let h = &v.tracks[0].header;
        assert!(h.contains("outside the PPU data range: 2 before, 0 after (Fit all shows the 1 interpolated); 1 of them extrapolated (not covered by Fit all)"), "{h}");
    }

    #[test]
    fn ruler_labels_before_the_capture_start_carry_one_minus_sign() {
        use ps3dk_prof_viewer::model::TimeBase;
        use ps3dk_prof_viewer::timeline::ruler;
        let tb = TimeBase { freq_hz: Some(1_000_000_000) }; // 1 tick = 1 ns
        let v = View { t0: -300_000_000.0, dt: 400_000_000.0 };
        let labels: Vec<String> = ruler(&v, tb, 1500.0, 90.0).into_iter().map(|(_, l)| l).collect();
        assert!(labels.iter().any(|l| l.starts_with('-') && l.ends_with(" ms")), "{labels:?}");
        assert!(labels.iter().any(|l| l.starts_with('+')), "{labels:?}");
        assert!(labels.iter().all(|l| !l.contains("+-")), "{labels:?}");
    }

    #[test]
    fn fit_all_reaches_bars_outside_the_ppu_data() {
        let bytes = f2_bytes(cm_ext());
        let doc = Document::from_bytes(PathBuf::from("t.prof"), &bytes);
        let a = doc.analysis().unwrap();
        let mut st = TimelineState::new(a);
        st.w32 = true;
        st.sync_foreign(&doc.foreign_tl, a);
        let fv = st.foreign.clone().unwrap();
        let (ilo, ihi) = fv.interp_extent.expect("interpolated bars exist");
        let ppu = st.tl.length();
        assert!(ihi > ppu, "the RSX bounds end after the PPU data: {ihi} vs {ppu}");
        assert!(st.view.t0 + st.view.dt < ihi, "PPU-only: the window cannot reach them");
        st.axis_all = true;
        st.sync_foreign(&doc.foreign_tl, a);
        assert!(st.view.t0 <= ilo.min(0.0) && st.view.t0 + st.view.dt >= ihi, "{:?}", st.view);
        // every interpolated bar of every track is now drawable inside the chart
        for t in &fv.tracks {
            let l = layout(t, &st.view, 1000.0, QueryCaps::default(), true);
            let n: usize = l.draws.iter().map(|d| match d.kind { FDrawKind::Dense { count } => count, _ => 1 }).sum();
            assert_eq!(n, t.bars.len(), "{}", t.label);
            assert!(l.draws.iter().all(|d| d.w1 <= 1000.0 && !d.arrow_right), "{l:?}");
        }
        // a bar mapped before the PPU data (as on the E1-x capture) is reached by the same axis rule
        let mut tl = ps3dk_prof_viewer::timeline::timeline(a);
        tl.extra = Some((-5000.0, -4000.0));
        let v = View::fit(&tl);
        assert!(v.t0 <= -5000.0 && v.t0 + v.dt >= tl.length());
        // and switching it off returns to the PPU data
        st.axis_all = false;
        st.sync_foreign(&doc.foreign_tl, a);
        assert_eq!(st.view, View::fit(&st.tl));
        assert!(st.tl.extra.is_none());
    }
}

// ---- Frames page graph modes: the SPU/RSX lane side (crate::graphs) ---------------------------------------------

mod graph_modes {
    use super::*;
    use ps3dk_prof_viewer::foreign::{build_dist, dist_cost, DistWork, DIST_OPS};
    use ps3dk_prof_viewer::graphs::{lane_cell, lane_point, ppu_point, runs, smooth, Cell, LinePoint};

    /// Owner 1 with `n` measured frames of 150 ticks (ids 0..n) and the final UNKNOWN frame n.
    fn owner_n(n: u64) -> (Analysis, FramesIndex) {
        let tbs: Vec<u64> = (0..=n).map(|k| 150 * k).collect();
        let a = analysed(&owner_bytes(&tbs, false, cm_ext()));
        let ix = FramesIndex::build(&a);
        (a, ix)
    }

    #[test]
    fn lane_points_use_the_upper_bound_keep_open_and_infinite_ends_and_break_elsewhere() {
        let (a, ix) = owner_n(4);
        let o = ix.owner(1).unwrap();
        let r = Ok(handcrafted(vec![
            cell(0, Some(bnd(rat(10, 1), false, Some(rat(30, 1))))),
            cell(1, Some(bnd(rat(0, 1), true, Some(rat(5, 1))))),
            cell(2, Some(bnd(rat(40, 1), false, None))),
            // frame 3: nothing included (an excluded-only cell)
            cell(3, None),
        ]));
        let v = build(&a, &ix, 1, &r, &Labels::build(&a), Budget::Fps30);
        let l = &v.lanes[0];
        assert_eq!(lane_point(o, l, 0), LinePoint::At { y: 30.0, band: Some((10.0, false)) }, "hi, with lo..hi");
        assert_eq!(lane_point(o, l, 1), LinePoint::At { y: 5.0, band: Some((0.0, true)) }, "the open lo is kept");
        assert_eq!(lane_point(o, l, 2), LinePoint::Unbounded { lo: 40.0, lo_open: false }, "+inf: no line point");
        assert_eq!(lane_point(o, l, 3), LinePoint::NonePlaced, "nothing included is never 0");
        assert_eq!(lane_point(o, l, 4), LinePoint::NoInterval, "the final frame");
        // The line runs over frames 0..1 and breaks at the infinite bound and after it.
        let rr = runs((0..5).map(|i| (i, lane_point(o, l, i).y())));
        assert_eq!(rr, vec![vec![(0, 30.0), (1, 5.0)]]);
        // Smoothing never bridges the break either.
        let s = smooth(0..5, 4, |i| lane_point(o, l, i).y());
        assert_eq!(s, vec![Some((30.0, 1)), Some((17.5, 2)), None, None, None]);
    }

    #[test]
    fn frame_k_is_the_same_slot_on_the_ppu_and_every_lane() {
        let (a, ix) = owner_n(3);
        let o = ix.owner(1).unwrap();
        // Two lanes, each with one bar on a different frame; neither is summed with the other or the PPU.
        let mut ff = handcrafted(vec![cell(1, Some(bnd(rat(7, 1), false, Some(rat(9, 1)))))]);
        let mut second = ff.lanes[0].clone();
        second.source = SourceKey::Lane(LANE);
        second.frames = vec![cell(2, Some(bnd(rat(3, 1), false, Some(rat(4, 1)))))];
        ff.lanes.push(second);
        let v = build(&a, &ix, 1, &Ok(ff), &Labels::build(&a), Budget::Fps30);
        let (rsx, spu) = (&v.lanes[0], &v.lanes[1]);
        for k in 0..3usize {
            assert_eq!(o.slots[k].frame, k as u64);
            assert_eq!(ppu_point(o, k), LinePoint::At { y: 150.0, band: None }, "the PPU's own value");
        }
        assert_eq!(lane_point(o, rsx, 1).y(), Some(9.0), "frame 1's RSX bar is at slot 1");
        assert_eq!(lane_point(o, spu, 2).y(), Some(4.0), "frame 2's SPU bar is at slot 2");
        assert_eq!(lane_point(o, rsx, 2), LinePoint::NonePlaced);
        assert_eq!(lane_point(o, spu, 1), LinePoint::NonePlaced);
        assert_eq!(lane_cell(o, rsx, 1), Cell::Within);
        assert_eq!(lane_cell(o, rsx, 0), Cell::NonePlaced);
        assert_eq!(lane_cell(o, rsx, 3), Cell::NoInterval);
    }

    #[test]
    fn lane_cells_carry_the_exact_verdicts() {
        let (a, ix) = owner_n(4);
        let o = ix.owner(1).unwrap();
        let b = (FREQ / 30) as i64; // the 30 FPS budget in ticks, exact
        let r = Ok(handcrafted(vec![
            cell(0, Some(bnd(rat(b + 1, 1), false, Some(rat(b + 2, 1))))),
            cell(1, Some(bnd(rat(1, 1), false, Some(rat(b, 1))))),
            cell(2, Some(bnd(rat(1, 1), false, Some(rat(b + 1, 1))))),
            cell(3, Some(bnd(rat(1, 1), false, Some(Rat { num: BigInt::from(1) << 600u32, den: BigInt::from(1) })))),
        ]));
        let v = build(&a, &ix, 1, &r, &Labels::build(&a), Budget::Fps30);
        let l = &v.lanes[0];
        let cells: Vec<Cell> = (0..5).map(|i| lane_cell(o, l, i)).collect();
        assert_eq!(cells, vec![Cell::Over, Cell::Within, Cell::Undetermined, Cell::Unavailable, Cell::NoInterval]);
        let v = build(&a, &ix, 1, &r, &Labels::build(&a), Budget::None);
        assert_eq!(lane_cell(o, &v.lanes[0], 0), Cell::NoVerdict, "no budget: no verdict");
    }

    #[test]
    fn lane_distribution_is_an_order_statistic_interval_with_exclusions_counted() {
        // 25 measured frames; bars on 24 of them (frame 3 has nothing included), the final frame has no interval.
        let (_a, ix) = owner_n(25);
        let mut cells = Vec::new();
        for k in 0..25i64 {
            if k == 3 {
                continue;
            }
            // lo = k, hi = 100 - k (so the hi order is the reverse of the frame order); frame 24's hi is +inf
            let hi = if k == 24 { None } else { Some(rat(100 - k, 1)) };
            cells.push(cell(k as u64, Some(bnd(rat(k, 1), k == 0, hi))));
        }
        let v = build_dist(&ix, 1, &Ok(handcrafted(cells)), DIST_OPS);
        let d = v.lanes[0].as_ref().unwrap();
        assert_eq!((d.bars, d.none_placed, d.no_interval), (24, 1, 1));
        let row = |label: &str| d.rows.iter().find(|r| r.label == label).unwrap().value.clone().unwrap();
        // los ascending: 0,1,2,4,..,24; his ascending: 77..97 except 97 (k=3), then 98, 99, 100, +inf
        let lo_at = |rank: usize| [0, 1, 2].iter().copied().chain(4..25).nth(rank - 1).unwrap() as f64;
        let his: Vec<i64> = (0..24i64).filter(|&k| k != 3).map(|k| 100 - k).rev().collect();
        let hi_at = |rank: usize| his.get(rank - 1).map(|&h| h as f64);
        assert_eq!((row("min").lo, row("min").hi), (0.0, Some(77.0)));
        assert_eq!(his.len(), 23);
        // p50: rank ceil(50 * 24 / 100) = 12; p90: rank ceil(21.6) = 22; p99: rank ceil(23.76) = 24 = max
        assert_eq!((row("p50").lo, row("p50").hi), (lo_at(12), hi_at(12)));
        assert_eq!((row("p90").lo, row("p90").hi), (lo_at(22), hi_at(22)));
        assert_eq!((row("p99").lo, row("p99").hi), (24.0, None), "the infinite hi sorts last");
        assert_eq!(row("max"), row("p99"));
        assert!(row("max").text.contains("+inf) ms"), "{}", row("max").text);
        // The p50 interval contains the p50 of any per-frame values inside the bars (here the midpoints).
        let mut mids: Vec<f64> = (0..24i64).filter(|&k| k != 3).map(|k| (k + 100 - k) as f64 / 2.0).collect();
        mids.push(1e9);
        mids.sort_by(|x, y| x.partial_cmp(y).unwrap());
        let p50 = mids[11];
        assert!(row("p50").lo <= p50 && p50 <= row("p50").hi.unwrap());
        // Below 20 bars the percentiles are refused; min and max stay.
        let few = build_dist(&ix, 1, &Ok(handcrafted(vec![cell(0, Some(bnd(rat(1, 1), false, Some(rat(2, 1)))))])), DIST_OPS);
        let d = few.lanes[0].as_ref().unwrap();
        assert_eq!((d.bars, d.none_placed, d.no_interval), (1, 24, 1));
        let labels: Vec<&str> = d.rows.iter().map(|r| r.label).collect();
        assert_eq!(labels, ["min", "p50", "p90", "p99", "max"]);
        assert_eq!(d.rows[1].value, Err("1 frames with a bar, percentiles need 20".to_string()));
        assert!(d.rows[0].value.is_ok() && d.rows[4].value.is_ok());
        // No bar at all: no rows, every measured frame counted as excluded, nothing drawn as 0.
        let none = build_dist(&ix, 1, &Ok(handcrafted(vec![])), DIST_OPS);
        let d = none.lanes[0].as_ref().unwrap();
        assert_eq!((d.bars, d.none_placed, d.no_interval, d.rows.len()), (0, 25, 1, 0));
    }

    /// `n` lanes, lane i holding `cells_of(i)` (the reader's lane shape, all on one owner).
    fn lanes_of(n: usize, cells_of: impl Fn(usize) -> Vec<FrameCell>) -> ForeignFrames {
        let mut ff = handcrafted(vec![]);
        let proto = ff.lanes[0].clone();
        ff.lanes = (0..n)
            .map(|i| {
                let mut l = proto.clone();
                l.source = SourceKey::Lane(i as u32 + 100);
                l.frames = cells_of(i);
                l
            })
            .collect();
        ff
    }

    #[test]
    fn many_empty_and_sparse_lanes_over_a_large_owner_cost_their_cells_not_the_frames() {
        // 5000 measured frames + the final one; 300 lanes, every 10th with one bar, the rest empty.
        let (_a, ix) = owner_n(5000);
        let o = ix.owner(1).unwrap();
        assert_eq!((o.slots.len(), o.placeable()), (5001, 5000), "eligibility counted once with the slots");
        let ff = lanes_of(300, |i| if i % 10 == 0 { vec![cell((i % 5000) as u64, Some(bnd(rat(1, 1), false, Some(rat(2, 1)))))] } else { vec![] });
        let d = build_dist(&ix, 1, &Ok(ff), DIST_OPS);
        // Independent charge: an empty lane 4; a one-cell lane 4 + 1 * (1 + clog2(5002) = 13 + 2) + 3 + 0 + 40 = 63.
        assert_eq!(dist_cost(0, 5001), 4);
        assert_eq!(dist_cost(1, 5001), 63);
        assert_eq!(d.used, 270 * 4 + 30 * 63, "bounded by the cells, never lanes x frames");
        assert_eq!(d.work.visits, 30, "only the 30 cells exist to visit");
        for (i, l) in d.lanes.iter().enumerate() {
            let l = l.as_ref().unwrap();
            let bars = u64::from(i % 10 == 0);
            assert_eq!((l.bars, l.none_placed, l.no_interval), (bars, 5000 - bars, 1), "lane {i}");
        }
    }

    #[test]
    fn the_distribution_budget_is_exact_and_one_short_refuses_visibly() {
        let (_a, ix) = owner_n(25);
        let cells: Vec<FrameCell> = (0..25).map(|k| cell(k, Some(bnd(rat(k as i64, 1), false, Some(rat(k as i64 + 5, 1)))))).collect();
        // 4 + 25 * (1 + clog2(27) = 5 + 2) + 3 * 25 + 2 * clog2(25) (= 5) * (3 * 25 + 25) + 40 = 1319
        assert_eq!(dist_cost(25, 26), 1319);
        let two = || Ok(lanes_of(2, |_| cells.clone()));
        let exact = build_dist(&ix, 1, &two(), 2 * 1319);
        assert!(exact.lanes.iter().all(|l| l.is_ok()) && exact.used == 2 * 1319, "{exact:?}");
        let short = build_dist(&ix, 1, &two(), 2 * 1319 - 1);
        assert!(short.lanes[0].is_ok());
        let why = short.lanes[1].as_ref().unwrap_err();
        assert_eq!(why, "distribution refused: budget (2637 operations; this lane needs 1319)");
        let first = build_dist(&ix, 1, &two(), 1318);
        assert!(first.lanes.iter().all(|l| l.as_ref().is_err_and(|w| w.starts_with("distribution refused: budget"))),
            "a refused lane refuses every later one: {first:?}");
        assert_eq!(first.used, 0);
    }

    #[test]
    fn a_bound_over_the_bit_ceiling_refuses_only_its_lane() {
        let (_a, ix) = owner_n(4);
        let huge = Rat { num: BigInt::from(1) << 600u32, den: BigInt::from(1) };
        let ff = lanes_of(2, |i| {
            if i == 0 {
                vec![cell(0, Some(bnd(rat(1, 1), false, Some(huge.clone()))))]
            } else {
                vec![cell(1, Some(bnd(rat(1, 1), false, Some(rat(3, 1)))))]
            }
        });
        let d = build_dist(&ix, 1, &Ok(ff), DIST_OPS);
        assert_eq!(d.lanes[0], Err("distribution refused: a bound over the bit ceiling".to_string()));
        assert_eq!(d.lanes[1].as_ref().unwrap().bars, 1);
        // a reader refusal: no lanes at all (the page shows the refusal itself)
        assert!(build_dist(&ix, 1, &Err(Refusal::NoExtension), DIST_OPS).lanes.is_empty());
    }

    #[test]
    fn only_the_modes_that_show_it_build_the_distribution() {
        use eframe::egui;
        use ps3dk_prof_viewer::frames_ui::{draw, FramesState};
        use ps3dk_prof_viewer::graphs::GraphMode;
        let a = analysed(&owner_bytes(&[0, 150, 300], false, cm_ext()));
        let ix = FramesIndex::build(&a);
        let ctx = egui::Context::default();
        ps3dk_ui::install(&ctx);
        let run = |slot: &ForeignSlot, st: &mut FramesState| {
            for _ in 0..3 {
                let _ = ctx.run(egui::RawInput::default(), |ctx| {
                    egui::CentralPanel::default().show(ctx, |p| {
                        draw(p, &a, &ix, st, None, Some(slot));
                    });
                });
            }
        };
        let dist = |s: &ForeignSlot| s.counters.dist.load(std::sync::atomic::Ordering::Relaxed);
        let slot = ForeignSlot::default();
        let mut st = FramesState::default();
        st.reset(Some(&ix));
        run(&slot, &mut st);
        assert_eq!((st.mode, dist(&slot)), (GraphMode::Bars, 0), "Bars never builds it");
        st.mode = GraphMode::VerdictMap;
        run(&slot, &mut st);
        assert_eq!(dist(&slot), 0, "nor the verdict map");
        st.mode = GraphMode::Lines;
        st.full_scale = true;
        run(&slot, &mut st);
        assert_eq!(dist(&slot), 0, "nor Lines at the Max scale");
        st.full_scale = false;
        run(&slot, &mut st);
        assert_eq!(dist(&slot), 1, "the Lines p99 fit builds it once");
        st.mode = GraphMode::Distribution;
        run(&slot, &mut st);
        assert_eq!(dist(&slot), 1, "cached per reader entry");
    }

    #[test]
    fn the_distribution_wording_names_its_population_and_omissions() {
        use ps3dk_prof_viewer::foreign::LaneDist;
        use ps3dk_prof_viewer::frames_ui::{dist_population, DIST_CAPTION};
        use ps3dk_prof_viewer::graphs::scale_caption;
        for w in ["INCLUDED start-attributed", "AMBIGUOUS", "EXTRAPOLATED", "exclusive-unavailable", "frames with nothing included", "not of all"] {
            assert!(DIST_CAPTION.contains(w), "{w:?} missing from {DIST_CAPTION}");
        }
        assert!(!DIST_CAPTION.contains("true per-frame work lies"), "no claim about all work");
        let line = dist_population(&LaneDist { bars: 3, none_placed: 5, no_interval: 1, rows: vec![] });
        assert_eq!(
            line,
            "population: included start-attributed top-level work on 3 frames with a bar. Omitted: 5 measured frames \
             with nothing included, 1 frames without an interval, and every AMBIGUOUS, EXTRAPOLATED or \
             exclusive-unavailable pair (in no bar; counted in the lane footer)"
        );
        let max = scale_caption(true, "9 ms", 0);
        assert!(max.contains("every finite value fits") && max.contains("+inf") && max.contains("arrow"), "{max}");
        assert!(!max.contains("every value fits"), "{max}");
        let fit = scale_caption(false, "9 ms", 0);
        assert!(fit.contains("Fit p99") && fit.contains("+inf") && fit.contains("arrow"), "{fit}");
        for c in [DIST_CAPTION.to_string(), line, max, fit] {
            assert!(!c.contains("  "), "{c}");
        }
    }

    #[test]
    fn zero_budget_and_lanes_after_exhaustion_visit_and_allocate_nothing() {
        let (_a, ix) = owner_n(25);
        let cells: Vec<FrameCell> = (0..25).map(|k| cell(k, Some(bnd(rat(k as i64, 1), false, Some(rat(k as i64 + 5, 1)))))).collect();
        let three = || Ok(lanes_of(3, |_| cells.clone()));
        let none = build_dist(&ix, 1, &three(), 0);
        assert!(none.lanes.iter().all(|l| l.is_err()));
        assert_eq!((none.used, none.work), (0, DistWork::default()), "a zero budget touches no cell and allocates nothing");
        // One lane fits: the later two are refused with no visit and no allocation of their own.
        let one = build_dist(&ix, 1, &three(), dist_cost(25, 26));
        assert!(one.lanes[0].is_ok() && one.lanes[1].is_err() && one.lanes[2].is_err());
        let alone = build_dist(&ix, 1, &Ok(lanes_of(1, |_| cells.clone())), DIST_OPS);
        assert_eq!(one.work, alone.work, "the refused lanes added nothing");
        assert_eq!(one.work.visits, 25);
    }

    #[test]
    fn an_admitted_sort_stays_within_its_charge_exactly_and_one_short_does_no_work() {
        let (_a, ix) = owner_n(25);
        // 25 bars in a scrambled order (7 is coprime with 25), so both sorts really merge.
        let cells: Vec<FrameCell> = (0..25u64)
            .map(|k| {
                let v = ((k * 7) % 25) as i64;
                cell(k, Some(bnd(rat(v, 1), false, Some(rat(100 - v, 1)))))
            })
            .collect();
        let need = dist_cost(25, 26);
        let ok = build_dist(&ix, 1, &Ok(lanes_of(1, |_| cells.clone())), need);
        let d = ok.lanes[0].as_ref().unwrap();
        let row = |label: &str| d.rows.iter().find(|r| r.label == label).unwrap().value.clone().unwrap();
        assert_eq!((row("min").lo, row("min").hi), (0.0, Some(76.0)), "sorted: smallest lo 0, smallest hi 100 - 24");
        assert_eq!((row("max").lo, row("max").hi), (24.0, Some(100.0)));
        let w = ok.work;
        assert!(w.compares > 0 && w.visits == 25, "{w:?}");
        assert!(w.visits + w.allocs + 3 * w.compares <= ok.used, "counted work {w:?} within the charge {}", ok.used);
        let short = build_dist(&ix, 1, &Ok(lanes_of(1, |_| cells.clone())), need - 1);
        assert!(short.lanes[0].is_err());
        assert_eq!(short.work, DistWork::default(), "one short: refused before any cell");
    }

    /// Draws the Frames page three times in `mode` (Fit p99) and returns the last Drawn.
    fn draw_mode(a: &Analysis, ix: &FramesIndex, slot: &ForeignSlot, mode: ps3dk_prof_viewer::graphs::GraphMode) -> ps3dk_prof_viewer::frames_ui::Drawn {
        use eframe::egui;
        use ps3dk_prof_viewer::frames_ui::{draw, Drawn, FramesState};
        let ctx = egui::Context::default();
        ps3dk_ui::install(&ctx);
        let mut st = FramesState::default();
        st.reset(Some(ix));
        st.mode = mode;
        let mut last = Drawn::default();
        for _ in 0..3 {
            let _ = ctx.run(egui::RawInput::default(), |ctx| {
                egui::CentralPanel::default().show(ctx, |p| {
                    last = draw(p, a, ix, &mut st, None, Some(slot));
                });
            });
        }
        last
    }

    #[test]
    fn a_budget_refusal_is_shown_as_the_fit_fallback_in_lines_and_distribution() {
        use ps3dk_prof_viewer::graphs::{fit_fallback_notes, GraphMode};
        let a = analysed(&owner_bytes(&[0, 150, 300], false, cm_ext()));
        let ix = FramesIndex::build(&a);
        for mode in [GraphMode::Lines, GraphMode::Distribution] {
            // A zero distribution budget: both lanes (the SPU lane and the RSX domain) are refused.
            let d = draw_mode(&a, &ix, &ForeignSlot::with_dist_ops(0), mode);
            assert_eq!(d.fit_fallback, 2, "{mode:?}");
            // The default budget: nothing refused, no fallback.
            assert_eq!(draw_mode(&a, &ix, &ForeignSlot::default(), mode).fit_fallback, 0, "{mode:?}");
        }
        // Bars never builds the distribution, so it never shows the note.
        assert_eq!(draw_mode(&a, &ix, &ForeignSlot::with_dist_ops(0), GraphMode::Bars).fit_fallback, 0);
        let v = build_dist(&ix, 1, &Ok(lanes_of(1, |_| vec![cell(0, Some(bnd(rat(1, 1), false, Some(rat(2, 1)))))])), 0);
        let notes = fit_fallback_notes(&["RSX domain 3"], &v.lanes, false);
        assert_eq!(
            notes,
            vec!["RSX domain 3: distribution refused: budget (0 operations; this lane needs 52); the Fit p99 scale uses \
                  this lane's finite maximum (conservative fallback)"
                .to_string()]
        );
        assert!(fit_fallback_notes(&["RSX domain 3"], &v.lanes, true).is_empty(), "Max needs no fallback");
        let cap = ps3dk_prof_viewer::graphs::scale_caption(false, "9 ms", 2);
        assert!(cap.contains("except 2 lane(s) whose distribution was refused") && cap.contains("finite maximum"), "{cap}");
        assert!(!cap.contains("  "), "{cap}");
    }

    #[test]
    fn a_bit_ceiling_refusal_is_shown_as_the_fit_fallback_on_the_draw_path() {
        use ps3dk_prof_viewer::graphs::GraphMode;
        let a = analysed(&owner_bytes(&[0, 150, 300], false, cm_ext()));
        let ix = FramesIndex::build(&a);
        let huge = Rat { num: BigInt::from(1) << 600u32, den: BigInt::from(1) };
        let result = || Ok(handcrafted(vec![cell(1, Some(bnd(rat(1, 1), false, Some(huge.clone()))))]));
        for mode in [GraphMode::Lines, GraphMode::Distribution] {
            let slot = ForeignSlot::seeded(&a, 1, false, result(), None);
            let d = draw_mode(&a, &ix, &slot, mode);
            assert_eq!(d.fit_fallback, 1, "{mode:?}");
            assert_eq!(slot.counters.reader.load(std::sync::atomic::Ordering::Relaxed), 0, "the seeded result was used");
            let dv = slot.dist(&ix, 1, false).unwrap();
            assert_eq!(dv.lanes, vec![Err("distribution refused: a bound over the bit ceiling".to_string())]);
        }
    }
}

// ---- Timeline SPU/RSX rows: stretches no placed pair's outer extent meets are marked, not blank ----------------

mod tl_gaps {
    use super::*;
    use ps3dk_prof::clockmap::{End, Projection};
    use ps3dk_prof::foreign_timeline::{FtBar, FtCounts, FtSource, FtSourceState};
    use ps3dk_prof_viewer::model::TimeBase;
    use ps3dk_prof_viewer::timeline_foreign::{
        build_track, build_track_axis, build_track_with, gaps_line, track_gaps, FTrack, ViewBudgets, GAP_FRACTION,
        GAP_MAX, Rel, TILES,
    };

    fn e(v: Option<i64>, neg: bool) -> End {
        match v {
            Some(v) => End::At { value: rat(v, 1), open: false },
            None if neg => End::NegInf,
            None => End::PosInf,
        }
    }

    /// A bar whose outer extent is [lo, hi] (None: unbounded on that side).
    fn bar(lo: Option<i64>, hi: Option<i64>) -> FtBar {
        FtBar {
            epoch: 1,
            image: Some(0),
            desc: 0,
            depth: 0,
            begin: Projection { lo: e(lo, true), hi: e(lo.map(|v| v + 1), false), interpolated: true },
            end: Projection { lo: e(hi.map(|v| v - 1), true), hi: e(hi, false), interpolated: true },
            inclusive: 1,
            exclusive: Some(1),
            extrapolated: false,
        }
    }

    fn src(bars: Vec<FtBar>, counts: FtCounts) -> FtSource {
        FtSource { source: SourceKey::Lane(LANE), epochs: vec![], counts, state: FtSourceState::Bars(bars) }
    }

    fn on_axis(bars: Vec<FtBar>, end: f64) -> FTrack {
        build_track_axis(&src(bars, FtCounts::default()), 0, Some(end), ViewBudgets::default())
    }

    /// No shaded gap meets any bar's outer extent, and every gap lies inside the axis.
    fn assert_disjoint(t: &FTrack, axis: (f64, f64)) {
        for &(g0, g1) in &t.gaps {
            assert!(axis.0 <= g0 && g0 < g1 && g1 <= axis.1, "gap {g0}..{g1} outside the axis {axis:?}");
            for b in &t.bars {
                let lo = match b.b_lo { Rel::At(v) => v, _ => f64::NEG_INFINITY };
                let hi = match b.e_hi { Rel::At(v) => v, _ => f64::INFINITY };
                assert!(!(lo < g1 && g0 < hi), "gap {g0}..{g1} meets the outer extent {lo}..{hi}");
            }
        }
    }

    #[test]
    fn internal_gaps_respect_the_threshold_and_nesting() {
        // Extent 0..1000 (no axis): threshold 1000 / 50 = 20; a 15-tick gap is not listed, nesting opens none.
        let t = build_track(&src(vec![bar(Some(0), Some(100)), bar(Some(10), Some(40)), bar(Some(115), Some(130)), bar(Some(600), Some(1000))], FtCounts::default()), 0);
        assert_eq!(t.gaps, vec![(130.0, 600.0)]);
        assert_eq!(GAP_FRACTION, 50.0);
        assert_disjoint(&t, (0.0, 1000.0));
    }

    #[test]
    fn a_half_unbounded_bar_covers_the_edge_on_its_open_side_only() {
        // Review repro 1: [300, +inf] and [600, 700] on [0, 1000]: the only gap is [0, 300], never [0, 600].
        let t = on_axis(vec![bar(Some(300), None), bar(Some(600), Some(700))], 1000.0);
        assert_eq!(t.gaps, vec![(0.0, 300.0)]);
        assert_disjoint(&t, (0.0, 1000.0));
        // Review repro 2: [-inf, 700] and [300, 400]: the only gap is [700, 1000], never [400, 1000].
        let t = on_axis(vec![bar(None, Some(700)), bar(Some(300), Some(400))], 1000.0);
        assert_eq!(t.gaps, vec![(700.0, 1000.0)]);
        assert_disjoint(&t, (0.0, 1000.0));
    }

    #[test]
    fn all_half_unbounded_and_outside_axis_bars() {
        // Every bar half-unbounded: [-inf, 100] and [400, +inf] leave exactly [100, 400].
        let t = on_axis(vec![bar(None, Some(100)), bar(Some(400), None)], 1000.0);
        assert_eq!(t.gaps, vec![(100.0, 400.0)]);
        assert_disjoint(&t, (0.0, 1000.0));
        // Both unbounded ends overlapping: no gap.
        let t = on_axis(vec![bar(None, Some(500)), bar(Some(400), None)], 1000.0);
        assert!(t.gaps.is_empty(), "{:?}", t.gaps);
        // ... and without an axis the same bars list only the internal stretch.
        let t = build_track(&src(vec![bar(None, Some(100)), bar(Some(400), None)], FtCounts::default()), 0);
        assert_eq!(t.gaps, vec![(100.0, 400.0)]);
        // Bars wholly before and after the axis: the whole axis is one gap, clipped.
        let t = on_axis(vec![bar(Some(-500), Some(-400)), bar(Some(1200), Some(1300))], 1000.0);
        assert_eq!(t.gaps, vec![(0.0, 1000.0)]);
        assert_disjoint(&t, (0.0, 1000.0));
        // Bars straddling both axis ends with a hole between.
        let t = on_axis(vec![bar(Some(-100), Some(200)), bar(Some(900), Some(1500))], 1000.0);
        assert_eq!(t.gaps, vec![(200.0, 900.0)]);
        assert_disjoint(&t, (0.0, 1000.0));
        // Late start and early stop: both edges.
        let t = on_axis(vec![bar(Some(300), Some(400)), bar(Some(600), Some(700))], 1000.0);
        assert_eq!(t.gaps, vec![(0.0, 300.0), (400.0, 600.0), (700.0, 1000.0)]);
        // Out of the reader's order: no gaps rather than a gap that could cross a bar.
        assert!(on_axis(vec![bar(Some(600), Some(700)), bar(Some(0), Some(100))], 1000.0).gaps.is_empty());
        assert!(on_axis(vec![], 1000.0).gaps.is_empty());
    }

    #[test]
    fn gaps_are_capped_and_charged_in_the_view_build_budget() {
        // Bars every 3000 ticks over [0, 100000] (threshold 2000): 33 stretches of 2999 (the trailing 999 is below
        // the threshold), never more than GAP_MAX.
        let bars: Vec<FtBar> = (0..34).map(|i| bar(Some(i * 3000), Some(i * 3000 + 1))).collect();
        let t = on_axis(bars, 100_000.0);
        assert_eq!(t.gaps.len(), 33);
        assert!(t.gaps.len() <= GAP_MAX);
        assert_eq!(track_gaps(&t.bars, Some((0.0, 100_000.0))), t.gaps);
        // The charge: 6 per bar, then bars + GAP_MAX for the gap pass, before the tiles (one bar, one tile).
        let s = src(vec![bar(Some(0), Some(10))], FtCounts::default());
        let need = 6 + (1 + GAP_MAX as u64) + TILES as u64 + 1;
        let exact = build_track_with(&s, 0, ViewBudgets { build_ops: need });
        assert!(exact.refusal.is_none(), "{:?}", exact.refusal);
        let short = build_track_with(&s, 0, ViewBudgets { build_ops: 6 + GAP_MAX as u64 });
        assert!(short.refusal.as_deref().unwrap_or("").contains("view build budget"));
        assert!(short.gaps.is_empty() && short.bars.is_empty(), "a refused gap pass leaves nothing drawn");
    }

    #[test]
    fn the_gap_line_names_its_scope_and_the_unplaced_coverage_separately() {
        let counts = FtCounts {
            placed: 4,
            not_placed: 7,
            censored: 2,
            unpaired: 1,
            open: None,
            loss_records: Some(3),
            no_inventory_epochs: 1,
            ..FtCounts::default()
        };
        let t = build_track_axis(&src(vec![bar(Some(300), Some(400)), bar(Some(600), Some(700))], counts), 0, Some(1000.0), ViewBudgets::default());
        let line = gaps_line(&t, TimeBase { freq_hz: None }).unwrap();
        assert!(line.starts_with("3 shaded stretch(es) (longest 300.0 ticks): no placed completed pair's outer extent"), "{line}");
        assert!(line.contains("work that was not placed may still fall there"), "{line}");
        assert!(
            line.contains("there. Not shown on this row at all: 7 pairs not placed, 2 censored, 1 unpaired, unknown open, loss records 3, 1 epoch(s) without an inventory"),
            "{line}"
        );
        for banned in ["idle", "not an unmapped", "unmapped epoch"] {
            assert!(!line.contains(banned), "{banned:?} in {line}");
        }
        assert!(!line.contains("  "), "{line}");
        assert!(gaps_line(&on_axis(vec![bar(Some(0), Some(1000))], 1000.0), TimeBase { freq_hz: None }).is_none());
    }
}
