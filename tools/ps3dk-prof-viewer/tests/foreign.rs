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

// ---- codex 12:24 carries: the no-clock draw path, excluded/candidate-only cells, NoInterval slot kinds -----------

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
