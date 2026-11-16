//! Automatic RSX observation (file_flags bit 7, chunk types 34-38): strict decode, refusals, the capture's own clock
//! map and the per-frame verdict.  Contract: docs/design/ps3dk-prof-auto-rsx.md.  The fixtures under
//! tests/fixtures/auto are captures written by the host runtime controls (tests/host/ps3dk-prof-runtime/
//! auto-runtime-test.py) with synthetic device facts; the synthetic files below are encoded byte by byte here.
mod common;
mod fifo_support;
use common::*;
use fifo_support::*;
use ps3dk_prof::auto_rsx::{analyze, write_json, write_text, Config, MapState, Report, Verdict};
use ps3dk_prof::{reader, Rule};

#[test]
fn timeline_busy_uses_absolute_qualified_endpoint_bounds() {
    use ps3dk_prof::{auto_rsx::busy_timeline, clockmap::{self, End, Meter, Obs, Solved}};
    let cap = reader::read(&fixture("console-r2")).unwrap();
    let report = analyze(&cap);
    let timeline = busy_timeline(&cap, &report);
    assert!(timeline.notice.is_none(), "{:?}", timeline.notice);
    assert!(!timeline.bars.is_empty());
    let observations: Vec<_> = report.clocks.iter().filter(|a| a.flags == 1).map(|a| Obs {
        f: a.raw.into(), lo: a.tb_before.into(), hi: a.tb_after.into(),
    }).collect();
    let mut meter = Meter::unlimited();
    let Solved::Compatible(model) = clockmap::solve(&mut meter, &observations).unwrap() else { panic!() };
    for bar in timeline.bars {
        for (raw, bounds) in [(bar.raw_begin, bar.begin), (bar.raw_end, bar.end)] {
            let projection = model.project(&mut meter, raw.into()).unwrap();
            assert!(projection.interpolated);
            let (End::At { value: lo, .. }, End::At { value: hi, .. }) = (projection.lo, projection.hi) else { panic!() };
            assert_eq!(bounds.0.to_string(), lo.floor().to_string());
            assert_eq!(bounds.1.to_string(), hi.ceil().to_string());
        }
        assert!(report.frames.iter().any(|f| (f.owner, f.frame) == (bar.owner, bar.frame)));
    }
}

#[test]
fn timeline_busy_merges_execution_intervals_and_refuses_unsafe_evidence() {
    use ps3dk_prof::auto_rsx::*;
    let mut cap = reader::read(&fixture("console-r2")).unwrap();
    let report = analyze(&cap);
    let Some(MapState::Qualified(map)) = report.map else { panic!() };
    let x = u64::try_from(map.fmin).unwrap() + 10;
    assert!(u64::try_from(map.fmax).unwrap() > x + 300);
    let inv = cap.auto.as_mut().unwrap();
    let seed = inv.ops.iter().find(|o| o.covered() && o.raw_valid() && o.token().is_some()).unwrap().clone();
    let op = |b, e, frame, flags, kind| {
        let mut o = seed.clone();
        o.raw_begin = b; o.raw_end = e; o.frame = frame; o.flags = flags; o.kind = kind;
        o
    };
    let flags = OP_TOKEN | OP_RAW_VALID;
    inv.ops = vec![
        op(x, x + 100, seed.frame, flags, KIND_CLEAR),
        op(x + 50, x + 200, seed.frame, flags, KIND_CALL),
        op(x + 250, x + 300, seed.frame + 1, flags, KIND_ARRAY),
        op(x, x + 300, seed.frame, flags | OP_CLOCK_INVALID, KIND_CLEAR),
        op(x, x + 300, seed.frame, flags, KIND_FRAME),
        op(0, u64::MAX, seed.frame, flags, KIND_CLEAR),
        op(x + 300, x, seed.frame, flags, KIND_CLEAR),
    ];
    let report = analyze(&cap);
    let timeline = busy_timeline(&cap, &report);
    assert_eq!(timeline.bars.len(), 2);
    assert_eq!((timeline.bars[0].raw_begin, timeline.bars[0].raw_end, timeline.bars[0].operations), (x, x + 200, 2));
    assert_eq!(timeline.bars[1].frame, seed.frame + 1);
    assert_eq!(timeline.omitted, 3); // invalid clock, outside support, reversed interval; FRAME is not an op

    for flag in [ST_SEGMENT_REPLAY, ST_RESOURCE_CONFLICT, ST_CONTEXT_RESET] {
        let mut bad = report.clone();
        bad.status.as_mut().unwrap().flags |= flag;
        let refused = busy_timeline(&cap, &bad);
        assert!(refused.bars.is_empty());
        assert!(refused.notice.as_ref().unwrap().contains("invalidated"));
    }
    cap.auto.as_mut().unwrap().clocks.clear();
    let unavailable = busy_timeline(&cap, &analyze(&cap));
    assert!(unavailable.bars.is_empty());
    assert_eq!(unavailable.notice.as_deref(), Some("clock map not qualified: RSX lanes unavailable"));
}

const AUTO_FLAGS: u32 = 1 | 64 | 128;

fn fixture(name: &str) -> Vec<u8> {
    let p = format!("{}/tests/fixtures/auto/{name}.prof", env!("CARGO_MANIFEST_DIR"));
    std::fs::read(&p).unwrap_or_else(|e| panic!("{p}: {e}"))
}
fn read_fixture(name: &str) -> Report {
    analyze(&reader::read(&fixture(name)).unwrap_or_else(|m| panic!("{name}: {m:?}")))
}
fn rule(f: &File) -> Rule {
    reader::read(&f.bytes()).expect_err("must refuse").rule
}

// ---- independent AUTO encoders -------------------------------------------------------------------------------------

#[derive(Clone)]
struct ACfg {
    flags: u16,
    words: [u32; 14],
    freq: u64,
    allocated: u64,
    ceiling: u64,
    premise: u32,
    lease: u32,
}
impl Default for ACfg {
    fn default() -> Self {
        ACfg {
            flags: 0,
            //       domain epoch owner gen kinds forms first count  op   clock pairs meta frames width
            words: [D, 1, OWNER, 1, 7, 15, 1024, 1024, 239, 238, 504, 1024, 256, 128],
            freq: FREQ,
            allocated: 240_384,
            ceiling: 2 * 1024 * 1024,
            premise: 3,
            lease: 1,
        }
    }
}
impl ACfg {
    fn refused(reason: u32, rc: i32) -> ACfg {
        let mut w = [0u32; 14];
        w[4] = reason;
        w[5] = rc as u32;
        ACfg { flags: 1, words: w, freq: FREQ, allocated: 0, ceiling: 0, premise: 0, lease: 0 }
    }
    fn chunk(&self) -> Chunk {
        let mut b = Buf::default();
        b.u16(1).u16(self.flags).u32(1);
        for w in self.words {
            b.u32(w);
        }
        b.u64(self.freq).u64(self.allocated).u64(self.ceiling).u32(self.premise).u32(self.lease);
        b.0.extend_from_slice(&[0x2a; 32]);
        b.0.extend_from_slice(&[0x63; 32]);
        if self.flags == 0 && self.words[4] == 1023 {
            // the tag writer's reserves: whole-CALL 32, ranges 16 (504 pairs), then zero
            let pairs = self.words[10];
            let (call, ranges) = if pairs >= 128 { (32, 16) } else { (pairs / 4, pairs / 8) };
            b.u32(call).u32(ranges).u64(0);
        }
        chunk(34, b.0)
    }
}

#[derive(Clone, Copy)]
struct AOp {
    id: u64,
    owner: u32,
    kind: u16,
    flags: u16,
    frame: u64,
    tb: (u64, u64),
    raw: (u64, u64),
    args: [u32; 7],
    reports: (u32, u32),
    serials: (u32, u32),
    generation: u32,
    r0: u32,
    r1: u32,
    published: u64,
    /// the 63/11 build tail: build frame, build owner, build flags, reserved
    tail: (u64, u32, u16, u16),
}
fn op(id: u64, frame: u64, raw: (u64, u64)) -> AOp {
    let s = 2 * id as u32 - 1;
    AOp {
        id,
        owner: OWNER,
        kind: 3,
        flags: 3,
        frame,
        tb: (10, 20),
        raw,
        args: [6, 0, 0, 0, 0, 0, 0xf3],
        reports: (1040 + 2 * (id as u32 % 500), 1041 + 2 * (id as u32 % 500)),
        serials: (s, s + 1),
        generation: 1,
        r0: 0,
        r1: 0,
        published: u64::from(s + 1),
        tail: (0, 0, 0, 0),
    }
}
fn ops_w(list: &[AOp], width: u32) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u32(list.len() as u32).u32(width);
    for o in list {
        b.u64(o.id).u32(o.owner).u16(o.kind).u16(o.flags).u64(o.frame).u64(o.tb.0).u64(o.tb.1).u64(o.raw.0)
            .u64(o.raw.1).u64(0x1234);
        for a in o.args {
            b.u32(a);
        }
        b.u32(o.r0).u32(o.reports.0).u32(o.reports.1).u32(o.serials.0).u32(o.serials.1).u32(o.generation).u32(o.r1)
            .u64(o.published);
        if width >= 144 {
            b.u64(o.tail.0).u32(o.tail.1).u16(o.tail.2).u16(o.tail.3);
        }
        if width == 160 {
            b.u32(0).u32(0).u32(0).u32(0); // tag, fp_offset, depth, reserved
        }
    }
    chunk(35, b.0)
}
fn ops(list: &[AOp]) -> Chunk {
    ops_w(list, 128)
}

#[derive(Clone, Copy)]
struct ARow {
    flags: u16,
    owner: u32,
    frame: u64,
    tb: (u64, u64),
    unsupported: u64,
    lost: u64,
    rows: [[u64; 12]; 3],
    present: (u32, u64),
}
/// A complete row with `clears` observed clears.
fn row(frame: u64, tb: (u64, u64), clears: u64) -> ARow {
    let mut rows = [[0u64; 12]; 3];
    rows[2][0] = clears;
    rows[2][1] = clears;
    ARow { flags: 3, owner: OWNER, frame, tb, unsupported: 0, lost: 0, rows, present: (0, 0) }
}
fn coverage(r: &ARow) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(r.flags).u32(1).u32(r.owner).u32(r.present.0).u64(r.frame).u64(r.tb.0).u64(r.tb.1).u64(r.unsupported)
        .u64(r.lost).u64(r.present.1);
    for k in r.rows {
        for v in k {
            b.u64(v);
        }
    }
    chunk(36, b.0)
}

fn status(flags: u16, n: [u64; 15]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(flags).u32(1);
    for v in n {
        b.u64(v);
    }
    chunk(37, b.0)
}

#[derive(Clone, Copy)]
struct AClk {
    flags: u16,
    id: u32,
    slot: u32,
    serial: u32,
    tb: (u64, u64),
    raw: u64,
    repeated: u64,
    labels: (u32, u32),
    frame: u64,
}
fn anchor(id: u32, raw: u64, tb: (u64, u64)) -> AClk {
    AClk { flags: 1, id, slot: 1024 + (id - 1) % 16, serial: id, tb, raw, repeated: raw, labels: (id, id), frame: 0 }
}
fn clock(a: &AClk) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(a.flags).u32(1).u32(1).u32(a.id).u32(0).u32(a.slot).u32(a.serial).u32(1).u64(a.tb.0).u64(a.tb.1)
        .u64(a.raw).u64(a.repeated).u32(a.labels.0).u32(a.labels.1).u64(a.frame).u32(OWNER).u32(0)
        .u64(a.tb.1 - a.tb.0);
    chunk(38, b.0)
}

/// Nine anchors on t = 2 f: raw 1000 + 50000 i, PPU bracket [t - 1, t + 1].
fn anchors() -> Vec<AClk> {
    (0..9u64).map(|i| {
        let f = 1000 + 50_000 * i;
        anchor(i as u32 + 1, f, (2 * f - 1, 2 * f + 1))
    }).collect()
}

fn status_for(n_ops: u64, rows: u64, clocks: u64, published: u64) -> [u64; 15] {
    [n_ops, n_ops, n_ops, 0, 0, rows, 0, 0, 0, clocks, clocks, 0, 0, published, 0]
}

/// The base with FIFO + AUTO flags and an attached CONFIG, then `rest`.
fn auto_file(rest: Vec<Chunk>) -> File {
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(ACfg::default().chunk());
    f.chunks.extend(rest);
    f
}

fn frame_wrap(seq0: u64, id: u64, t: u64, p1: u64, p2: u64, frame: u64) -> Vec<Ev> {
    wrap(seq0, id, t, p1, p2).into_iter().map(|mut e| { e.frame = frame; e }).collect()
}

/// Four frames of 100000 PPU ticks: RSX-bound, PPU-bound, mixed and unresolved.
fn four_frames() -> File {
    let mut evs = frame_wrap(1, 1, 20_000, 20_000, 10_000, 0);
    evs.extend(frame_wrap(7, 2, 220_000, 1000, 1000, 2));
    let list = [op(1, 0, (6000, 36_000)), op(2, 1, (60_000, 61_000)), op(3, 2, (110_000, 150_000)),
        op(4, 3, (160_000, 200_000))];
    let mut rest = vec![events(&evs), St::new(1, 12, 12, 0, 12, 2, 2, 0).chunk(), ops(&list)];
    for (i, t) in [10_000u64, 110_000, 210_000, 310_000].into_iter().enumerate() {
        rest.push(coverage(&row(i as u64, (t, t + 100_000), 1)));
    }
    rest.extend(anchors().iter().map(clock));
    rest.push(status(1, status_for(4, 4, 9, 8)));
    auto_file(rest)
}

// ---- synthetic verdicts ---------------------------------------------------------------------------------------------

#[test]
fn verdicts_follow_the_evidence() {
    let r = analyze(&reader::read(&four_frames().bytes()).expect("reads"));
    assert_eq!(r.state, "FINAL");
    let Some(MapState::Qualified(m)) = &r.map else { panic!("map {:?}", r.map) };
    assert_eq!(m.anchors, 9);
    assert!(m.decisive.0 > 0 && m.decisive.1 > 0);
    let v: Vec<Verdict> = r.frames.iter().map(|f| f.verdict).collect();
    // frame 1: the RSX is not limiting, and without presentation evidence that is never PPU-bound
    assert_eq!(v, [Verdict::RsxBound, Verdict::Unknown, Verdict::Mixed, Verdict::Unknown], "{:#?}", r.frames);
    assert!(r.frames[0].reason.starts_with("fetch:"), "{}", r.frames[0].reason);
    assert_eq!(r.frames[1].reason, ps3dk_prof::auto_rsx::NOT_DISTINGUISHED);
    assert!(r.frames[3].reason.starts_with("unresolved"));
    let (lo, hi) = r.frames[0].rsx_span_ppu.unwrap();
    assert!(lo <= 60_000 && 60_000 <= hi && lo >= 50_000, "{lo}..{hi}");
    assert!(r.frames.iter().all(|f| f.verdict != Verdict::Pacing), "no presentation evidence: never PACING");
    assert!(r.frames.iter().all(|f| f.verdict != Verdict::PpuBound), "no busy evidence: never PPU-bound");
}

/// four_frames with the 15/11 CONFIG masks, every row carrying FR_PRESENT with `present[i]` (waits, ticks).
fn four_frames_present(present: [(u32, u64); 4]) -> File {
    let mut f = four_frames();
    let at = f.chunks.iter().position(|c| c.ty == 34).unwrap();
    f.chunks[at] = seg_cfg().chunk();
    let rows: Vec<usize> = f.chunks.iter().enumerate().filter(|(_, c)| c.ty == 36).map(|(i, _)| i).collect();
    for (k, (&i, t)) in rows.iter().zip([10_000u64, 110_000, 210_000, 310_000]).enumerate() {
        let mut x = row(k as u64, (t, t + 100_000), 1);
        x.flags |= 64;
        x.present = present[k];
        f.chunks[i] = coverage(&x);
    }
    f
}

#[test]
fn poll_intervals_never_establish_pacing() {
    // Review finding 1: a 15/11 PRESENT row records the time from the first pending poll to the done poll.  Two
    // nonblocking polls around application work give the same number as a polling wait, so 60% of the frame between
    // polls is an observation interval, not a waiting lower bound: UNKNOWN, never PACING.
    let r = analyze(&reader::read(&four_frames_present([(0, 0), (1, 60_000), (0, 0), (0, 0)]).bytes()).expect("reads"));
    let v: Vec<Verdict> = r.frames.iter().map(|f| f.verdict).collect();
    assert_eq!(v, [Verdict::RsxBound, Verdict::Unknown, Verdict::Mixed, Verdict::Unknown], "{:#?}", r.frames);
    assert!(r.frames[1].reason.starts_with("presentation polling observed; waiting not established"),
        "{}", r.frames[1].reason);
}

#[test]
fn pacing_needs_presentation_waits() {
    let r = analyze(&reader::read(&four_frames_present([(0, 0), (1, 60_000), (0, 0), (0, 0)]).bytes()).expect("reads"));
    assert_eq!(r.frames[1].reason, format!("{} (writer without flip-wait scopes)", ps3dk_prof::auto_rsx::POLLING_ONLY));
    let p = r.frames[1].presentation.as_ref().unwrap();
    assert_eq!((p.legacy_polls, p.flip_wait, p.cadence), (Some((1, 60_000)), None, None));
    let r = analyze(&reader::read(&four_frames_present([(0, 0), (0, 0), (0, 0), (0, 0)]).bytes()).unwrap());
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    assert_eq!(r.frames[1].reason, ps3dk_prof::auto_rsx::NOT_DISTINGUISHED);
    // presentation fields need FR_PRESENT, ticks need a wait, and a first-writer capture knows none of it
    let mut x = row(0, (10, 20), 0);
    x.present = (1, 5);
    assert_eq!(rule(&seg_file(vec![coverage(&x)])), Rule::AutoCoverage);
    x.flags |= 64;
    x.present = (0, 5);
    assert_eq!(rule(&seg_file(vec![coverage(&x)])), Rule::AutoCoverage);
    x.present = (1, 5);
    assert!(reader::read(&seg_file(vec![coverage(&x)]).bytes()).is_ok());
    assert_eq!(rule(&auto_file(vec![coverage(&x)])), Rule::AutoCoverage);
}

#[test]
fn missing_inputs_stay_unknown() {
    // without the FIFO family there is no wait evidence
    let mut f = four_frames();
    f.flags = 1 | 128;
    f.chunks.retain(|c| !(31..=33).contains(&c.ty));
    let r = analyze(&reader::read(&f.bytes()).unwrap());
    assert!(r.frames.iter().all(|x| x.verdict == Verdict::Unknown));
    assert!(r.frames[0].reason.contains("no FIFO wait evidence"), "{}", r.frames[0].reason);
    // a failed anchor disqualifies the map; raw spans stay visible
    let mut f = four_frames();
    let at = f.chunks.iter().position(|c| c.ty == 38).unwrap() + 4;
    let mut a = anchors()[4];
    a.flags = 2;
    a.raw = 0;
    a.repeated = 0;
    f.chunks[at] = clock(&a);
    let r = analyze(&reader::read(&f.bytes()).unwrap());
    assert!(matches!(&r.map, Some(MapState::Unqualified(w)) if w.contains("anchor 5 failed (TIMEOUT)")), "{:?}", r.map);
    assert!(r.frames.iter().all(|x| x.verdict == Verdict::Unknown && x.rsx_span_raw.is_some()));
    // three anchors: too few coordinates
    let mut f = four_frames();
    let first = f.chunks.iter().position(|c| c.ty == 38).unwrap();
    f.chunks.drain(first + 3..first + 9);
    let last = f.chunks.len() - 1;
    f.chunks[last] = status(1, status_for(4, 4, 3, 8));
    let r = analyze(&reader::read(&f.bytes()).unwrap());
    assert!(matches!(&r.map, Some(MapState::Unqualified(w)) if w.contains("fewer than 4")));
    // an excluded operation in a frame
    let mut f = four_frames();
    let at = f.chunks.iter().position(|c| c.ty == 36).unwrap();
    let mut x = row(0, (10_000, 110_000), 1);
    x.rows[2][0] = 2;
    x.rows[2][2] = 1;
    f.chunks[at] = coverage(&x);
    let r = analyze(&reader::read(&f.bytes()).unwrap());
    assert_eq!(r.frames[0].verdict, Verdict::Unknown);
    assert!(r.frames[0].reason.contains("excluded"));
    // no FINAL status
    let mut f = four_frames();
    let last = f.chunks.len() - 1;
    f.chunks[last] = status(0, status_for(4, 4, 9, 8));
    let r = analyze(&reader::read(&f.bytes()).unwrap());
    assert!(r.state.starts_with("PROVISIONAL"));
    assert!(r.frames.iter().all(|x| x.verdict == Verdict::Unknown));
}

// ---- wire refusals --------------------------------------------------------------------------------------------------

#[test]
fn flag_gate() {
    let mut f = four_frames();
    f.flags = 128 | 64;
    assert_eq!(rule(&f), Rule::FileFlagsNonzero);
    let b = four_frames().bytes();
    let c = reader::read(&b[..13]).unwrap();
    assert!(c.auto.is_some() && c.fifo.is_some());
    let mut bad = b.clone();
    bad[12] = 128;
    let e = reader::read(&bad[..13]).unwrap_err();
    assert!(e.detail.contains("bits 6 and 7 need bit 0"), "{}", e.detail);
    // without bit 7 the AUTO chunk types stay unknown
    let mut f = four_frames();
    f.flags = 65;
    assert_eq!(rule(&f), Rule::UnknownChunkType);
}

#[test]
fn config_refusals() {
    let cases: Vec<(ACfg, &str)> = vec![
        ({ let mut c = ACfg::default(); c.words[7] = 1022; c }, "report count"),
        ({ let mut c = ACfg::default(); c.words[8] = 238; c }, "labels"),
        ({ let mut c = ACfg::default(); c.words[0] = 1027; c }, "RSX_TIMESTAMP"),
        ({ let mut c = ACfg::default(); c.words[2] = 9; c }, "not declared"),
        ({ let mut c = ACfg::default(); c.freq += 1; c }, "CLOCKS frequency"),
        ({ let mut c = ACfg::default(); c.lease = 0; c }, "lease policy"),
        ({ let mut c = ACfg::default(); c.flags = 2; c }, "variant"),
        ({ let mut c = ACfg::refused(11, 0); c.flags = 1; c }, "reason 11"),
        ({ let mut c = ACfg::refused(3, 0); c.words[0] = D; c }, "REFUSED AUTO_CONFIG carries"),
    ];
    for (cfg, want) in cases {
        let mut f = base(AUTO_FLAGS);
        f.chunks.push(cfg.chunk());
        let e = reader::read(&f.bytes()).expect_err(want);
        assert_eq!(e.rule, Rule::AutoConfig, "{want}");
        assert!(e.detail.contains(want), "{want}: {}", e.detail);
    }
    // a second CONFIG, and AUTO data before any CONFIG
    let mut f = auto_file(vec![ACfg::default().chunk()]);
    assert_eq!(rule(&f), Rule::AutoConfig);
    f = base(AUTO_FLAGS);
    f.chunks.push(status(1, [0; 15]));
    assert_eq!(rule(&f), Rule::AutoStatus);
}

#[test]
fn refused_config_variant() {
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(ACfg::refused(1, -5).chunk());
    f.chunks.push(status(1, [0; 15]));
    let r = analyze(&reader::read(&f.bytes()).unwrap());
    assert!(matches!(r.config, Some(Config::Refused { reason: 1, application_rc: -5, .. })));
    assert_eq!(r.state, "automatic observation unavailable: GCM_INIT_FAILED");
    assert!(r.frames.is_empty() && r.map.is_none());
    // REFUSED forbids every other AUTO chunk, and its status is FINAL with zero counters
    let mut g = base(AUTO_FLAGS);
    g.chunks.push(ACfg::refused(1, -5).chunk());
    g.chunks.push(ops(&[op(1, 0, (1, 2))]));
    assert_eq!(rule(&g), Rule::AutoOps);
    let mut g = base(AUTO_FLAGS);
    g.chunks.push(ACfg::refused(1, -5).chunk());
    let mut n = [0u64; 15];
    n[9] = 1;
    g.chunks.push(status(1, n));
    assert_eq!(rule(&g), Rule::AutoStatus);
    // nothing after FINAL
    f.chunks.push(status(1, [0; 15]));
    assert_eq!(rule(&f), Rule::AutoStatus);
}

#[test]
fn ops_refusals() {
    let base_ops = [op(1, 0, (6000, 36_000))];
    let mutate: Vec<(Box<dyn Fn(&mut AOp)>, &str)> = vec![
        (Box::new(|o| o.flags |= 64), "unknown flags"), // SEGMENT is not yet understood by this reader
        (Box::new(|o| o.flags = 1 | 2 | 4), "exactly one"),
        (Box::new(|o| o.flags = 1 | 2 | 16), "READ_REFUSED"),
        (Box::new(|o| o.flags = 1 | 4), "INCOMPLETE with raw"),
        (Box::new(|o| o.r0 = 1), "reserved"),
        (Box::new(|o| o.r1 = 1), "reserved"),
        (Box::new(|o| o.kind = 4), "kind 4"),
        (Box::new(|o| o.owner = 2), "frame owner"),
        (Box::new(|o| o.serials.1 += 1), "serials"),
        (Box::new(|o| o.reports = (1030, 1031)), "operation range"),
        (Box::new(|o| o.published = 1), "publication snapshot"),
        (Box::new(|o| o.raw = (9, 8)), "raw begin above"),
        (Box::new(|o| o.tb = (20, 10)), "CLOCK_INVALID"),
        (Box::new(|o| o.args[1] = 5), "unused by kind"),
        (Box::new(|o| o.generation = 2), "generation"),
    ];
    for (m, want) in mutate {
        let mut o = base_ops[0];
        m(&mut o);
        let f = auto_file(vec![ops(&[o])]);
        let e = reader::read(&f.bytes()).expect_err(want);
        assert_eq!(e.rule, Rule::AutoOps, "{want}");
        assert!(e.detail.contains(want), "{want}: {}", e.detail);
    }
    // ids strictly increase across chunks
    let f = auto_file(vec![ops(&[op(2, 0, (1, 2))]), ops(&[op(2, 0, (1, 2))])]);
    assert_eq!(rule(&f), Rule::AutoOps);
}

#[test]
fn coverage_status_and_clock_refusals() {
    let mut x = row(0, (10, 20), 1);
    x.rows[2][0] = 2;
    assert_eq!(rule(&auto_file(vec![coverage(&x)])), Rule::AutoCoverage);
    let mut x = row(0, (10, 20), 1);
    x.lost = 1;
    assert_eq!(rule(&auto_file(vec![coverage(&x)])), Rule::AutoCoverage);
    let mut x = row(0, (20, 10), 1);
    assert_eq!(rule(&auto_file(vec![coverage(&x)])), Rule::AutoCoverage);
    x.flags |= 16; // OPEN keeps both observed ticks
    assert!(reader::read(&auto_file(vec![coverage(&x)]).bytes()).is_ok());
    let x = row(3, (10, 20), 0);
    assert_eq!(rule(&auto_file(vec![coverage(&x), coverage(&x)])), Rule::AutoCoverage);
    // STATUS conservation and parsed totals
    let f = auto_file(vec![ops(&[op(1, 0, (1, 2))]), status(1, status_for(2, 0, 0, 2))]);
    assert_eq!(rule(&f), Rule::AutoStatus);
    let mut n = status_for(1, 0, 0, 2);
    n[4] = 1;
    let f = auto_file(vec![ops(&[op(1, 0, (1, 2))]), status(1, n)]);
    assert_eq!(rule(&f), Rule::AutoStatus);
    n[1] = 2;
    let f = auto_file(vec![ops(&[op(1, 0, (1, 2))]), status(1, n)]);
    assert_eq!(rule(&f), Rule::AutoStatus, "pending without LOWER_BOUND");
    assert!(reader::read(&auto_file(vec![ops(&[op(1, 0, (1, 2))]), status(3, n)]).bytes()).is_ok());
    // CLOCK: slots repeat, ids and serials never do
    let mut a = anchors();
    a[1].id = 1;
    a[1].serial = 1;
    assert_eq!(rule(&auto_file(a.iter().take(2).map(clock).collect())), Rule::AutoClock);
    let mut a = anchors();
    a[0].labels.1 = 0;
    assert_eq!(rule(&auto_file(vec![clock(&a[0])])), Rule::AutoClock);
    let mut a = anchors();
    a[0].slot = 1040;
    assert_eq!(rule(&auto_file(vec![clock(&a[0])])), Rule::AutoClock);
    let reused: Vec<AClk> = (1..=20u32).map(|i| anchor(i, 1000 * u64::from(i), (2000 * u64::from(i) - 1,
        2000 * u64::from(i) + 1))).collect();
    let c = reader::read(&auto_file(reused.iter().map(clock).collect()).bytes()).unwrap();
    let inv = c.auto.unwrap();
    assert_eq!(inv.clocks.len(), 20);
    assert_eq!(inv.clocks[16].slot, inv.clocks[0].slot);
}

// ---- host runtime capture fixtures ----------------------------------------------------------------------------------

#[test]
fn fixtures_from_the_host_runtime() {
    let r = read_fixture("noinit");
    assert!(matches!(r.config, Some(Config::Refused { reason: 10, .. })));
    assert_eq!(r.state, "automatic observation unavailable: NO_GCM_INIT_OBSERVED");
    let r = read_fixture("clockcap");
    assert_eq!(r.state, "FINAL");
    assert_eq!(r.clocks.len(), 36);
    assert!(r.clocks.iter().all(|a| a.outcome() == "VALID"));
    assert_eq!(r.clocks[16].slot, r.clocks[0].slot, "slots are reused");
    assert_eq!(r.frames.len(), 256);
    assert!(r.frames.iter().all(|f| f.verdict == Verdict::Unknown));
    let r = read_fixture("segments");
    assert_eq!(r.frames[0].unsupported, 5);
    assert_eq!(r.frames[1].unsupported, 1);
    assert!(r.frames[0].reason.contains("outside the observed population"), "{}", r.frames[0].reason);
    let r = read_fixture("frames");
    assert_eq!(r.frames[0].excluded, [0, 0, 4]);
    assert_eq!(r.ops_valid, 2);
    for name in ["ops", "conflict", "reset", "init", "clocks"] {
        let r = read_fixture(name);
        assert!(matches!(r.config, Some(Config::Attached(_))), "{name}");
    }
    for name in ["conflict", "reset"] {
        assert!(read_fixture(name).state.starts_with("FINAL (lower bounds)"), "{name}");
    }
    for name in ["stop", "fifopost"] {
        let r = read_fixture(name);
        assert!(r.status.as_ref().is_some_and(|s| s.flags & 8 != 0), "{name}: OWNER_UNACK");
    }
}

/// Captures written before the build-frame writer (CONFIG 15/11 and older) keep the exact JSON the previous
/// reader printed: the 63/11 fields (segment_replay, built, recorded, segment_*) never appear for them. The goldens
/// were produced by the reader at b451c6a5e; the per-reason exclusion columns added since are checked separately.
#[test]
fn auto_json_unchanged_for_older_captures() {
    let dir = format!("{}/tests/fixtures/auto", env!("CARGO_MANIFEST_DIR"));
    for name in ["segments", "d12-segments", "console-r2", "frames"] {
        let o = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof"))
            .args(["auto", "--json", &format!("{dir}/{name}.prof")]).output().unwrap();
        let mut got: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
        // the only addition since b451c6a5e: per-kind exclusion-reason columns on every frame row
        for fr in got["frames"].as_array_mut().unwrap() {
            let ex = fr.as_object_mut().unwrap().remove("exclusions").expect("exclusions on every frame");
            for kind in ["ARRAY", "INDEX", "CLEAR"] {
                let cols = ex[kind].as_object().unwrap();
                assert_eq!(cols.len(), 10, "{name}: ten reason columns");
                let sum: u64 = cols.values().map(|v| v.as_u64().unwrap()).sum();
                let k = ["ARRAY", "INDEX", "CLEAR"].iter().position(|x| *x == kind).unwrap();
                assert_eq!(sum, fr["excluded"][k].as_u64().unwrap(), "{name}: columns sum to excluded");
            }
        }
        let want: serde_json::Value =
            serde_json::from_slice(&std::fs::read(format!("{dir}/{name}.base-json.json")).unwrap()).unwrap();
        assert_eq!(got, want, "{name}: JSON differs from the previous reader");
        assert!(got.get("segment_replay").is_none(), "{name}: segment_replay outside 63/11");
    }
}

/// `auto --ops`: every retained record once, in emission order, intervals exactly raw_end - raw_begin, --frame
/// keeps only that frame token, and the usual exit codes (1 without automatic observation, 3 for bad arguments).
#[test]
fn cli_auto_ops() {
    let dir = format!("{}/tests/fixtures/auto", env!("CARGO_MANIFEST_DIR"));
    let run = |args: &[&str]| std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(args).output().unwrap();
    for name in ["segments", "segbuild", "console-r2"] {
        let f = format!("{dir}/{name}.prof");
        let o = run(&["auto", "--json", &f]);
        let parsed = serde_json::from_slice::<serde_json::Value>(&o.stdout).unwrap()["operations"]["parsed"].as_u64().unwrap();
        let o = run(&["auto", "--ops", "--json", &f]);
        assert_eq!(o.status.code(), Some(0), "{name}");
        let v: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
        assert_eq!(v["schema"], 1);
        let ops = v["ops"].as_array().unwrap();
        assert_eq!(ops.len() as u64, parsed, "{name}: every retained record listed once");
        assert!(ops.windows(2).all(|w| w[0]["id"].as_u64() < w[1]["id"].as_u64()), "{name}: emission order");
        for op in ops {
            if let Some(i) = op["interval"].as_u64() {
                assert_eq!(i, op["raw_end"].as_u64().unwrap() - op["raw_begin"].as_u64().unwrap());
                assert!(op["flags"].as_u64().unwrap() & 2 != 0, "{name}: an interval only for RAW_VALID");
            }
        }
        if let Some(fr) = ops.iter().find_map(|op| op["frame"].as_u64()) {
            let o = run(&["auto", "--ops", "--frame", &fr.to_string(), "--json", &f]);
            let v: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
            let sel = v["ops"].as_array().unwrap();
            assert!(!sel.is_empty() && sel.iter().all(|op| op["frame"].as_u64() == Some(fr)), "{name}: --frame filter");
            // gaps are in execution order: exactly one raw-valid record of the frame (the first to begin) has none
            let valid = sel.iter().filter(|op| op["interval"].is_u64()).count();
            let no_gap = sel.iter().filter(|op| op["interval"].is_u64() && op["gap"].is_null()).count();
            assert!(valid == 0 || no_gap == 1, "{name}: {no_gap} raw-valid records without a gap");
            let o = run(&["auto", "--ops", "--frame", &fr.to_string(), &f]);
            assert_eq!(o.status.code(), Some(0));
            assert!(String::from_utf8_lossy(&o.stdout).contains(&format!("frame {fr} ")) || sel.iter().all(|op| op["interval"].is_null()));
        }
    }
    assert_eq!(run(&["auto", "--ops", &format!("{dir}/noinit.prof")]).status.code(), Some(0));
    assert_eq!(run(&["auto", "--ops", "--frame", "x", &format!("{dir}/segments.prof")]).status.code(), Some(3));
    assert_eq!(run(&["auto", "--ops", "--bogus", &format!("{dir}/segments.prof")]).status.code(), Some(3));
}

/// Past the operation retention ceiling the reader no longer refuses the whole capture: frames that kept every
/// record keep exactly the verdict a full read gives them, and frames that lost records read UNKNOWN with the
/// ceiling as the reason.
#[test]
fn auto_ops_ceiling_keeps_retained_frames() {
    let dir = format!("{}/tests/fixtures/auto", env!("CARGO_MANIFEST_DIR"));
    for name in ["console-r2", "segbuild"] {
        let bytes = std::fs::read(format!("{dir}/{name}.prof")).unwrap();
        let full = analyze(&reader::read(&bytes).unwrap());
        let n = full.ops_parsed;
        assert!(n > 4, "{name}: fixture has records");
        let budgets = ps3dk_prof::auto_rsx::Budgets { max_ops: n - 1, ..Default::default() };
        let cap = reader::read_with_auto_budgets(&bytes, budgets).unwrap();
        let dropped = cap.auto.as_ref().unwrap().dropped_frames.clone();
        let part = analyze(&cap);
        assert!(part.refusal.is_none(), "{name}: no capture-wide refusal");
        assert_eq!(part.ops_dropped, 1, "{name}: dropped count");
        assert!(!dropped.is_empty(), "{name}: some frame lost records");
        assert_eq!(full.frames.len(), part.frames.len());
        let mut kept = 0;
        for (a, b) in full.frames.iter().zip(part.frames.iter()) {
            if dropped.contains(&(b.owner, b.frame)) {
                assert_eq!(b.verdict, Verdict::Unknown, "{name}: frame {} lost records", b.frame);
            } else if b.reason != a.reason {
                // a kept frame may only differ when it imports segment draws built in a frame that lost records
                assert!(b.reason.contains(ps3dk_prof::auto_rsx::OPS_PAST_CEILING), "{name}: frame {}: {} vs {}", b.frame, a.reason, b.reason);
            } else {
                assert_eq!(a.verdict, b.verdict);
                kept += 1;
            }
        }
        if name == "console-r2" {
            assert!(kept > 1, "{name}: frames before the ceiling keep their verdicts");
        }
    }
}

#[test]
fn cli_auto() {
    let dir = format!("{}/tests/fixtures/auto", env!("CARGO_MANIFEST_DIR"));
    let run = |args: &[&str]| std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(args).output().unwrap();
    let o = run(&["auto", &format!("{dir}/noinit.prof")]);
    assert_eq!(o.status.code(), Some(1));
    assert!(String::from_utf8_lossy(&o.stdout).contains("automatic observation unavailable: NO_GCM_INIT_OBSERVED"));
    let o = run(&["auto", "--json", &format!("{dir}/clockcap.prof")]);
    assert_eq!(o.status.code(), Some(0));
    let v: serde_json::Value = serde_json::from_slice(&o.stdout).unwrap();
    assert_eq!(v["state"], "FINAL");
    assert_eq!(v["frames"].as_array().unwrap().len(), 256);
    assert_eq!(v["frames"][0]["verdict"], "UNKNOWN");
    assert_eq!(v["clock_map"]["state"], "UNQUALIFIED");
    let o = run(&["auto", "--bad", "x"]);
    assert_eq!(o.status.code(), Some(3));
}

// ---- D1 segments and D2 frame marks (CONFIG masks 15/11) -----------------------------------------------------------

fn seg_cfg() -> ACfg {
    let mut c = ACfg::default();
    c.words[4] = 15;
    c.words[5] = 11;
    c
}
fn seg_file(rest: Vec<Chunk>) -> File {
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(seg_cfg().chunk());
    f.chunks.extend(rest);
    f
}
fn mark(id: u64, frame: u64, role: u32, raw: u64) -> AOp {
    let mut o = op(id, frame, (raw, raw + 1));
    o.kind = 4;
    o.args = [role, 0, 0, 0, 0, 0, 0];
    o
}
fn seg_op(id: u64, frame: u64, raw: (u64, u64), segment: u32, call: u32, serials: (u32, u32)) -> AOp {
    let mut o = op(id, frame, raw);
    o.flags |= 64;
    o.r0 = segment;
    o.r1 = call;
    o.serials = serials;
    o.published = u64::from(serials.1);
    o
}

#[test]
fn cross_frame_segments_get_no_verdict() {
    // Review 2026-11-12: segment A built in frame 0 (one observed CLEAR, one excluded for room), called in frame 1
    // while B is built there (one observed CLEAR), B called in frame 2. Frame 1's one CLEAR record (A) and its one
    // observed CLEAR (B) are different operations; with qualified clocks and FIFO waits that matched as RSX-bound
    // and lost frame 0's exclusion. Every frame with segment records is UNKNOWN.
    let list = vec![mark(1, 1, 1, 5000), seg_op(2, 1, (6000, 36_000), 1, 1, (3, 4)), mark(3, 1, 2, 90_000),
        seg_op(4, 2, (100_000, 101_000), 2, 1, (7, 8))];
    let mut f0 = row(0, (0, 10_000), 1);
    f0.rows[2][0] = 2;
    f0.rows[2][2] = 1;
    let mut rest = vec![events(&frame_wrap(1, 1, 20_000, 20_000, 10_000, 1)), St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(),
        ops(&list)];
    rest.push(coverage(&f0));
    rest.push(coverage(&row(1, (10_000, 110_000), 1)));
    rest.push(coverage(&row(2, (110_000, 120_000), 0)));
    rest.extend(anchors().iter().map(clock));
    rest.push(status(1, status_for(4, 3, 9, 8)));
    let r = analyze(&reader::read(&seg_file(rest).bytes()).expect("reads"));
    let f1 = r.frames.iter().find(|f| f.frame == 1).expect("frame 1");
    assert_eq!((f1.records[2], f1.observed[2], f1.segment_ops), (1, 1, 1));
    assert_ne!(f1.verdict, Verdict::RsxBound, "{}", f1.reason);
    assert_eq!(f1.verdict, Verdict::Unknown);
    assert!(f1.reason.ends_with(ps3dk_prof::auto_rsx::SEGMENT_POPULATION), "{}", f1.reason);
    let f2 = r.frames.iter().find(|f| f.frame == 2).expect("frame 2");
    assert_eq!(f2.verdict, Verdict::Unknown);
    assert!(r.frames.iter().find(|f| f.frame == 0).unwrap().reason.contains("excluded"));
}

#[test]
fn segment_and_frame_records() {
    // frame 0: begin mark, two segment draws of one call (shared serial pair), end mark
    let mut list = vec![mark(1, 0, 1, 5000), seg_op(2, 0, (6000, 20_000), 1, 1, (3, 4)),
        seg_op(3, 0, (21_000, 36_000), 1, 1, (3, 4)), mark(4, 0, 2, 90_000)];
    list[3].serials = (5, 6);
    list[3].published = 6;
    let mut rest = vec![events(&frame_wrap(1, 1, 20_000, 20_000, 10_000, 0)), St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(),
        ops(&list)];
    rest.push(coverage(&row(0, (10_000, 110_000), 2)));
    rest.extend(anchors().iter().map(clock));
    rest.push(status(1, status_for(4, 1, 9, 6)));
    let r = analyze(&reader::read(&seg_file(rest).bytes()).expect("reads"));
    let f = &r.frames[0];
    assert_eq!(f.segment_ops, 2);
    assert_eq!(f.rsx_frame_raw, (Some(5000), Some(90_001)));
    let (lo, hi) = f.rsx_frame_ppu.expect("mapped");
    assert!(lo <= 170_002 && 170_002 <= hi, "{lo}..{hi}");
    // Even when built and called in one frame, segment records cannot prove that: no per-frame verdict.
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    assert!(f.reason.ends_with(ps3dk_prof::auto_rsx::SEGMENT_POPULATION), "{}", f.reason);
    let c = reader::read(&seg_file(vec![ops(&list)]).bytes()).unwrap();
    let ops_read = &c.auto.as_ref().unwrap().ops;
    assert_eq!((ops_read[1].segment, ops_read[1].call), (1, 1));
}

#[test]
fn old_format_refuses_segment_and_frame_records() {
    // A first-writer CONFIG (7/15) knows neither SEGMENT nor FRAME: both are refused, as an older reader would.
    let s = seg_op(1, 0, (1, 2), 1, 1, (1, 2));
    let e = reader::read(&auto_file(vec![ops(&[s])]).bytes()).unwrap_err();
    assert_eq!(e.rule, Rule::AutoOps);
    assert!(e.detail.contains("unknown flags"), "{}", e.detail);
    let e = reader::read(&auto_file(vec![ops(&[mark(1, 0, 1, 5)])]).bytes()).unwrap_err();
    assert!(e.detail.contains("kind 4"), "{}", e.detail);
    let e = reader::read(&auto_file(vec![status(1 | 2 | 64, [0; 15])]).bytes()).unwrap_err();
    assert_eq!(e.rule, Rule::AutoStatus);
    // the first reader (157d7d749) required masks 7/15 exactly, so it refuses a 15/11 CONFIG outright; this reader
    // accepts only the two defined pairs
    let mut c = seg_cfg();
    c.words[5] = 15;
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(c.chunk());
    assert_eq!(rule(&f), Rule::AutoConfig);
}

#[test]
fn segment_and_frame_refusals() {
    let cases: Vec<(AOp, &str)> = vec![
        (seg_op(1, 0, (1, 2), 0, 1, (1, 2)), "segment or call ordinal"),
        (seg_op(1, 0, (1, 2), 1, 0, (1, 2)), "segment or call ordinal"),
        ({ let mut o = mark(1, 0, 3, 5); o.flags = 3; o }, "FRAME mark"),
        ({ let mut o = mark(1, 0, 1, 5); o.flags = 2; o.owner = 0; o.frame = 0; o }, "FRAME mark"),
        ({ let mut o = mark(1, 0, 1, 5); o.args[6] = 1; o }, "FRAME mark"),
        ({ let mut o = seg_op(1, 0, (1, 2), 1, 1, (1, 2)); o.kind = 4; o.args = [1, 0, 0, 0, 0, 0, 0]; o }, "FRAME mark"),
    ];
    for (o, want) in cases {
        let e = reader::read(&seg_file(vec![ops(&[o])]).bytes()).expect_err(want);
        assert_eq!(e.rule, Rule::AutoOps, "{want}");
        assert!(e.detail.contains(want), "{want}: {}", e.detail);
    }
    // a never-called build: INCOMPLETE, call ordinal 0, and the quarantine status flag
    let mut o = seg_op(1, 0, (0, 0), 2, 0, (1, 2));
    o.flags = 64 | 4 | 16;
    o.owner = 0;
    o.frame = 0;
    o.published = 0;
    let mut n = status_for(1, 0, 0, 0);
    n[14] = 1;
    let c = reader::read(&seg_file(vec![ops(&[o.clone()]), status(1 | 2 | 64, n)]).bytes()).expect("reads");
    assert_eq!(c.auto.unwrap().last_status.unwrap().flags, 67);
    // FRAME_MARK_MISSED (128): accepted with the 15/11 masks, refused by a first-writer capture
    assert!(reader::read(&seg_file(vec![status(1 | 128, [0; 15])]).bytes()).is_ok());
    assert_eq!(rule(&auto_file(vec![status(1 | 128, [0; 15])])), Rule::AutoStatus);
}

#[test]
fn missed_frame_marks_leave_the_duration_unknown() {
    // frame 0 has only its begin mark; the status says marks were missed
    let list = [mark(1, 0, 1, 5000)];
    let rest = vec![ops(&list), coverage(&row(0, (10_000, 110_000), 0)), status(1 | 128, status_for(1, 1, 0, 2))];
    let r = analyze(&reader::read(&seg_file(rest).bytes()).expect("reads"));
    assert_eq!(r.frames[0].rsx_frame_raw, (Some(5000), None));
    assert_eq!(r.frames[0].rsx_frame_ppu, None);
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_text(&mut out, &r).unwrap();
    let text = String::from_utf8(out).unwrap();
    assert!(text.contains("FRAME marks missed"), "{text}");
    assert!(text.contains("rsx frame marks 5000..UNKNOWN ppu UNKNOWN"), "{text}");
}

#[test]
fn d1_d2_fixture_from_the_host_runtime() {
    let r = read_fixture("d12-segments");
    let Some(Config::Attached(a)) = &r.config else { panic!("attached") };
    assert_eq!((a.kind_mask, a.form_mask), (15, 11));
    assert!(r.status.as_ref().is_some_and(|s| s.flags & 64 != 0), "SEGMENT_QUARANTINE");
    assert_eq!(r.frames.len(), 2);
    assert_eq!(r.frames[0].segment_ops, 2);
    assert_eq!(r.frames[1].segment_ops, 1);
    assert!(r.frames.iter().all(|f| f.rsx_frame_raw.0.is_some() && f.rsx_frame_raw.1.is_some()));
    // frame 0 also holds the never-called build's observed clear, which has no record: UNKNOWN, not complete
    assert_eq!(r.frames[0].verdict, Verdict::Unknown);
}

#[test]
fn flip_frames_and_frame_source() {
    // a flip-opened row (FR_FLIP 32) and STATUS FRAMES_FLIP (256) with the 15/11 masks
    let mut x = row(0, (10, 20), 0);
    x.flags |= 32;
    let r = analyze(&reader::read(&seg_file(vec![coverage(&x), status(1 | 256, status_for(0, 1, 0, 0))]).bytes())
        .expect("reads"));
    assert_eq!(r.frames[0].flags & 32, 32);
    assert_eq!(ps3dk_prof::auto_rsx::frame_source(&r), "FLIP");
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_text(&mut out, &r).unwrap();
    let text = String::from_utf8(out).unwrap();
    assert!(text.contains("frames delimited by: FLIP"), "{text}");
    assert!(text.contains("note: An operation interval runs from the RSX reaching its begin marker"), "{text}");
    // both sources: MIXED, shown, not hidden
    let r = analyze(&reader::read(&seg_file(vec![coverage(&x), status(1 | 256 | 512, status_for(0, 1, 0, 0))]).bytes())
        .expect("reads"));
    assert!(ps3dk_prof::auto_rsx::frame_source(&r).starts_with("MIXED"));
    // neither: no frames seen
    let r = analyze(&reader::read(&seg_file(vec![status(1, [0; 15])]).bytes()).unwrap());
    assert!(ps3dk_prof::auto_rsx::frame_source(&r).starts_with("NONE"));
    // a first-writer capture knows neither the row flag nor the STATUS bits
    assert_eq!(rule(&auto_file(vec![coverage(&x)])), Rule::AutoCoverage);
    assert_eq!(rule(&auto_file(vec![status(1 | 256, [0; 15])])), Rule::AutoStatus);
    // and FRAMES_FLIP/EXPLICIT stay sticky across statuses
    let e = reader::read(&seg_file(vec![status(256, [0; 15]), status(1, [0; 15])]).bytes()).unwrap_err();
    assert_eq!(e.rule, Rule::AutoStatus);
}

#[test]
fn second_console_capture() {
    // flip frames, a qualified map, and the shutdown-closed final frame: no PPU-bound, the final frame named
    let r = read_fixture("console-r2");
    assert_eq!(ps3dk_prof::auto_rsx::frame_source(&r), "FLIP");
    assert!(matches!(&r.map, Some(MapState::Qualified(m)) if m.anchors == 16));
    assert_eq!(r.frames.len(), 120);
    assert!(r.frames.iter().all(|f| f.verdict == Verdict::Unknown && f.presentation.is_none()));
    // every frame of the call-return sample but the final one calls a segment, so it is refused before reconciliation
    assert_eq!(r.frames.iter().filter(|f| f.reason.ends_with(ps3dk_prof::auto_rsx::SEGMENT_POPULATION)).count(), 119);
    assert_eq!(r.frames[119].reason, ps3dk_prof::auto_rsx::FINAL_FRAME);
    assert!(r.frames[..112].iter().all(|f| f.rsx_frame_ppu.is_some()));
}

#[test]
fn batched_ops_chunk_and_console_fixture() {
    // one record per chunk (the first console capture) and many per chunk read the same records
    let list: Vec<AOp> = (1..=5).map(|i| op(i, 0, (100 * i, 100 * i + 10))).collect();
    let one = seg_file(list.iter().map(|o| ops(&[*o])).collect());
    let many = seg_file(vec![ops(&list)]);
    let a = reader::read(&one.bytes()).unwrap().auto.unwrap();
    let b = reader::read(&many.bytes()).unwrap().auto.unwrap();
    assert_eq!(a.ops, b.ops);
    assert_eq!(a.ops_parsed, 5);
    let r = read_fixture("console-r1");
    assert_eq!(r.state, "FINAL");
    assert_eq!((r.ops_parsed, r.ops_valid), (600, 600));
    assert!(r.frames.is_empty() && r.clocks.is_empty());
    assert!(ps3dk_prof::auto_rsx::frame_source(&r).starts_with("NONE"));
}

// ---- presentation writer (CONFIG masks 31/11): flip-wait scopes and polling cadence --------------------------------

/// The presentation block at 352: cadence runs, broken, spanning; spanning scopes; cadence ticks; timed-out scopes.
#[derive(Clone, Copy, Default)]
struct Blk {
    runs: u32,
    broken: u32,
    spanning: u32,
    scope_spanning: u32,
    ticks: u64,
    timeouts: u32,
    reserved: u32,
}
fn pres_cfg() -> ACfg {
    let mut c = seg_cfg();
    c.words[4] = 31;
    c
}
fn pres_file(rest: Vec<Chunk>) -> File {
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(pres_cfg().chunk());
    f.chunks.extend(rest);
    f
}
fn coverage_p(r: &ARow, k: &Blk) -> Chunk {
    let mut c = coverage(r);
    let mut b = Buf::default();
    b.u32(k.runs).u32(k.broken).u32(k.spanning).u32(k.scope_spanning).u64(k.ticks).u32(k.timeouts).u32(k.reserved);
    c.payload.extend_from_slice(&b.0);
    c
}
/// four_frames with the 31/11 masks; row i carries PRESENT, `scopes[i]` (scopes, ticks) at 12/56 and `blk[i]`.
fn four_frames_scopes(scopes: [(u32, u64); 4], blk: [Blk; 4]) -> File {
    let mut f = four_frames();
    let at = f.chunks.iter().position(|c| c.ty == 34).unwrap();
    f.chunks[at] = pres_cfg().chunk();
    let rows: Vec<usize> = f.chunks.iter().enumerate().filter(|(_, c)| c.ty == 36).map(|(i, _)| i).collect();
    for (k, (&i, t)) in rows.iter().zip([10_000u64, 110_000, 210_000, 310_000]).enumerate() {
        let mut x = row(k as u64, (t, t + 100_000), 1);
        x.flags |= 64;
        x.present = scopes[k];
        f.chunks[i] = coverage_p(&x, &blk[k]);
    }
    f
}

/// One 127/11 frame (PPU 10000..110000) with frame marks at RSX `marks` (raw; None = no end mark), one timed clear
/// at `timed`, one untimed clear in exclusion column `col`, a FIFO wait when `wait`, and flip-wait scopes `scope`.
fn bounded_frame(timed: (u64, u64), marks: (u64, Option<u64>), col: usize, wait: bool, scope: (u32, u64)) -> File {
    let mut list = vec![mark(1, 0, 1, marks.0), op(2, 0, timed)];
    if let Some(e) = marks.1 {
        list.push(mark(3, 0, 2, e));
    }
    let n = list.len() as u64;
    let mut x = row(0, (10_000, 110_000), 1);
    x.rows[2][0] = 2;
    x.rows[2][2 + col] = 1;
    x.flags |= 64;
    x.present = scope;
    let mut rest = vec![events(&frame_wrap(1, 1, 20_000, 20_000, 10_000, if wait { 0 } else { 7 })),
        St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(), ops_w(&list, 144), coverage_p(&x, &Blk::default())];
    // the same nine anchors with the bracketed label rule (seen below 2k-1, then at or above 2k)
    rest.extend(anchors().into_iter().map(|mut a| { a.labels = (0, 2 * a.id); clock(&a) }));
    rest.push(status(1, status_for(n, 1, 9, 2 * n)));
    brk_file(brk_cfg(), rest)
}

/// Untimed draws (report capacity and the other untimed-only columns) are decided on RSX busy bounds: the timed
/// span below, the frame marks above. A verdict both bounds give stands; otherwise, or without both marks, or with
/// an exclusion that invalidates the evidence, the frame stays UNKNOWN.
#[test]
fn untimed_draws_get_bounded_verdicts() {
    let cap = 1; // report capacity
    let verdict = |f: File| {
        let r = analyze(&reader::read(&f.bytes()).expect("reads"));
        let fr = r.frames[0].clone();
        (fr.verdict, fr.reason, fr.rsx_busy_bounds)
    };
    // a long timed span and a FIFO wait: RSX-bound whatever the untimed draw did
    let (v, why, b) = verdict(bounded_frame((6000, 36_000), (5000, Some(45_000)), cap, true, (0, 0)));
    assert_eq!(v, Verdict::RsxBound, "{why}");
    assert!(why.starts_with("1 covered draw(s) untimed; verdict holds for RSX busy in ["), "{why}");
    let (lo, hi) = b.expect("bounds");
    assert!(lo >= 50_000 && hi >= lo, "{lo}..{hi}");
    // the marks are close together (RSX short even at its upper bound), no FIFO wait, 60% inside flip waits: PACING
    let (v, why, b) = verdict(bounded_frame((6000, 7000), (5000, Some(20_000)), cap, false, (2, 60_000)));
    assert_eq!(v, Verdict::Pacing, "{why}");
    assert!(b.expect("bounds").1 < 50_000);
    // a short timed span but marks far apart: the untimed draw could make the RSX limiting or not
    let (v, why, b) = verdict(bounded_frame((6000, 7000), (5000, Some(45_000)), cap, true, (0, 0)));
    assert_eq!(v, Verdict::Unknown);
    assert!(why.starts_with("1 covered draw(s) untimed; RSX busy in [") && why.contains("does not decide"), "{why}");
    assert!(b.is_some());
    // no end mark: no upper bound
    let (v, why, b) = verdict(bounded_frame((6000, 36_000), (5000, None), cap, true, (0, 0)));
    assert_eq!(v, Verdict::Unknown);
    assert!(why.contains("give no upper bound"), "{why}");
    assert!(b.is_none());
    // every untimed-only column behaves the same; the evidence-invalidating ones keep the old refusal
    for col in 0..10 {
        let (v, why, _) = verdict(bounded_frame((6000, 36_000), (5000, Some(45_000)), col, true, (0, 0)));
        if ps3dk_prof::auto_rsx::UNTIMED_ONLY[col] {
            assert_eq!(v, Verdict::RsxBound, "column {col}: {why}");
        } else {
            assert_eq!(v, Verdict::Unknown, "column {col}");
            assert_eq!(why, "1 covered operation(s) excluded", "column {col}");
        }
    }
}

/// 127/11, two frames: G (frame 0) builds segment A (one clear timed, `g_col` an excluded column if any), F (frame 1)
/// calls A with `unrep` draws executed without a record, A's record at RSX `timed`, F's marks at `marks`, a FIFO wait in F.
fn seg_bounded(g_col: Option<usize>, unrep: u32, timed: (u64, u64), marks: (u64, u64), status_flags: u16) -> Report {
    let mut m1 = mark(1, 1, 1, marks.0);
    m1.serials = (1, 2);
    m1.published = 2;
    let a = built_op(2, 1, timed, 1, 1, 0);
    let mut m2 = mark(3, 1, 2, marks.1);
    m2.serials = (5, 6);
    m2.published = 6;
    let mut g = row(0, (10_000, 110_000), 1);
    if let Some(c) = g_col {
        g.rows[2][0] = 2;
        g.rows[2][2 + c] = 1;
    }
    let mut f = row(1, (110_000, 210_000), 0);
    f.flags |= 64;
    g.flags |= 64;
    let mut rest = vec![events(&frame_wrap(1, 1, 120_000, 20_000, 10_000, 1)), St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(),
        ops_b(&[m1, a, m2]), coverage_b(&g, 0), coverage_b(&f, unrep)];
    rest.extend(anchors().into_iter().map(|mut a| { a.labels = (0, 2 * a.id); clock(&a) }));
    rest.push(status(status_flags, status_for(3, 2, 9, 6)));
    analyze(&reader::read(&brk_file(brk_cfg(), rest).bytes()).expect("reads"))
}

/// Review case: G's only segment draw was excluded with column `g_col` (never recorded), F CALLs that build (one
/// unrepresented draw) and also has its own timed draw built in F, so no record of F names G.
fn seg_unnamed_build(g_col: usize) -> Report {
    let mut m1 = mark(1, 1, 1, 56_000);
    m1.serials = (1, 2);
    m1.published = 2;
    let a = built_op(2, 1, (60_000, 90_000), 1, 1, 1);
    let mut m2 = mark(3, 1, 2, 100_000);
    m2.serials = (5, 6);
    m2.published = 6;
    let mut g = row(0, (10_000, 110_000), 0);
    g.rows[2][0] = 1;
    g.rows[2][2 + g_col] = 1;
    let mut f = row(1, (110_000, 210_000), 1);
    f.flags |= 64;
    g.flags |= 64;
    let mut rest = vec![events(&frame_wrap(1, 1, 120_000, 20_000, 10_000, 1)), St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(),
        ops_b(&[m1, a, m2]), coverage_b(&g, 0), coverage_b(&f, 1)];
    rest.extend(anchors().into_iter().map(|mut a| { a.labels = (0, 2 * a.id); clock(&a) }));
    rest.push(status(1, status_for(3, 2, 9, 6)));
    analyze(&reader::read(&brk_file(brk_cfg(), rest).bytes()).expect("reads"))
}

/// A CALL of a wholly untimed build whose frame holds invalid evidence cannot be traced to that frame (no record
/// names it), so the calling frame refuses instead of taking a bounded verdict; untimed-only exclusions still bound.
#[test]
fn unnamed_invalid_build_refuses() {
    for (col, name) in [(9, "epoch invalid"), (6, "ownership uncertainty")] {
        let r = seg_unnamed_build(col);
        assert_eq!(r.frames[1].verdict, Verdict::Unknown, "{name}: {}", r.frames[1].reason);
        assert!(r.frames[1].reason.contains("have no record"), "{name}: {}", r.frames[1].reason);
    }
    let r = seg_unnamed_build(1);
    assert_eq!(r.frames[1].verdict, Verdict::RsxBound, "report capacity: {}", r.frames[1].reason);
}

/// Segment draws a CALL executed in F without a timed record (segment_unrepresented, including draws its build could
/// not time) are untimed for the bounded rule; a build frame whose only exclusions are untimed-only is still a source.
#[test]
fn segment_frames_get_bounded_verdicts() {
    let long = (60_000, 90_000);
    let wide = (56_000, 100_000);
    // an untracked or refused draw in F, long timed span: RSX-bound on both bounds
    let r = seg_bounded(None, 1, long, wide, 1);
    let f = &r.frames[1];
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.starts_with("1 covered draw(s) untimed; verdict holds"), "{}", f.reason);
    assert!(f.rsx_busy_bounds.is_some());
    // the same with a short timed span: the unrepresented draw could decide it
    let r = seg_bounded(None, 1, (60_000, 61_000), wide, 1);
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    assert!(r.frames[1].reason.contains("does not decide"), "{}", r.frames[1].reason);
    // A's second draw was excluded for report capacity while G built it (and counted as unrepresented in F):
    // G is still a source, F is bounded
    let r = seg_bounded(Some(1), 1, long, wide, 1);
    assert_eq!(r.frames[1].verdict, Verdict::RsxBound, "{}", r.frames[1].reason);
    // G with an evidence-invalidating column (ownership uncertainty) still refuses the import
    let r = seg_bounded(Some(6), 1, long, wide, 1);
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    // (it now refuses one check earlier: an invalid column anywhere means F's unrepresented draw cannot be traced)
    assert!(r.frames[1].reason.contains("have no record"), "{}", r.frames[1].reason);
    // a replayed segment: the sticky status keeps every frame UNKNOWN
    let r = seg_bounded(None, 1, long, wide, 1 | 1024);
    assert!(r.frames.iter().all(|f| f.verdict == Verdict::Unknown));
    assert_eq!(r.frames[1].reason, ps3dk_prof::auto_rsx::SEGMENT_REPLAY);
    // fully timed: today's verdict and reason, no bounds
    let r = seg_bounded(None, 0, long, wide, 1);
    assert_eq!(r.frames[1].verdict, Verdict::RsxBound, "{}", r.frames[1].reason);
    assert!(r.frames[1].reason.starts_with("fetch:"), "{}", r.frames[1].reason);
    assert!(r.frames[1].rsx_busy_bounds.is_none());
}

#[test]
fn pacing_needs_flip_wait_scopes() {
    let none = Blk::default();
    // frame 1 (RSX not limiting) spent 60% of its interval inside cellGcmWaitFlipComplete: PACING
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0), (2, 60_000), (0, 0), (0, 0)], [none; 4]).bytes())
        .expect("reads"));
    let v: Vec<Verdict> = r.frames.iter().map(|f| f.verdict).collect();
    assert_eq!(v, [Verdict::RsxBound, Verdict::Pacing, Verdict::Mixed, Verdict::Unknown], "{:#?}", r.frames);
    assert!(r.frames[1].reason.starts_with("presentation: flip-wait scope elapsed time, including scheduling and \
        polling, >= 60000 of 100000 PPU ticks (2 scope(s))"), "{}", r.frames[1].reason);
    // useful CPU work between every poll, every gap within the bound: cadence 60% of the frame, still UNKNOWN
    let cadence = Blk { runs: 1, ticks: 60_000, ..none };
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0); 4], [none, cadence, none, none]).bytes()).unwrap());
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    assert_eq!(r.frames[1].reason, ps3dk_prof::auto_rsx::POLLING_ONLY);
    // a poll, application work (a gap over the bound), a done poll: a broken run, UNKNOWN
    let broken = Blk { broken: 1, ..none };
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0); 4], [none, broken, none, none]).bytes()).unwrap());
    assert_eq!(r.frames[1].reason, ps3dk_prof::auto_rsx::POLLING_ONLY);
    // a short scope plus long cadence: still not a quarter of the frame inside scopes
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0), (1, 20_000), (0, 0), (0, 0)], [none, cadence, none,
        none]).bytes()).unwrap());
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    let p = r.frames[1].presentation.as_ref().unwrap();
    assert_eq!((p.flip_wait, p.cadence.map(|c| c.ticks), p.legacy_polls), (Some((20_000, 1)), Some(60_000), None));
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_json(&mut out, &r).unwrap();
    let j: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert_eq!(j["frames"][1]["presentation"]["flip_wait"]["ticks"], 20_000);
    assert_eq!(j["frames"][1]["presentation"]["polling_cadence"]["ticks"], 60_000);
}

#[test]
fn presentation_rows_refusals() {
    let mut x = row(0, (10, 20), 0);
    x.flags |= 64;
    let ok = Blk::default();
    assert!(reader::read(&pres_file(vec![coverage_p(&x, &ok)]).bytes()).is_ok());
    // a 31/11 row is 384 bytes; a 352-byte row is refused, and a 15/11 capture refuses the 384-byte row
    assert_eq!(rule(&pres_file(vec![coverage(&x)])), Rule::AutoCoverage);
    assert_eq!(rule(&seg_file(vec![coverage_p(&x, &ok)])), Rule::AutoCoverage);
    let cases: Vec<(Blk, (u32, u64), u16, &str)> = vec![
        (Blk { reserved: 1, ..ok }, (0, 0), 64, "reserved"),
        (Blk { runs: 1, ..ok }, (0, 0), 0, "without PRESENT"),
        (Blk { ticks: 5, ..ok }, (0, 0), 64, "without a complete run"),
        (Blk { timeouts: 2, ..ok }, (1, 5), 64, "timed-out"),
        (ok, (0, 5), 64, "presentation count"),
    ];
    for (k, present, flags, want) in cases {
        let mut y = row(0, (10, 20), 0);
        y.flags |= flags;
        y.present = present;
        let e = reader::read(&pres_file(vec![coverage_p(&y, &k)]).bytes()).expect_err(want);
        assert_eq!(e.rule, Rule::AutoCoverage, "{want}");
        assert!(e.detail.contains(want), "{want}: {}", e.detail);
    }
    // this reader accepts exactly the three defined mask pairs
    let mut c = pres_cfg();
    c.words[4] = 23;
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(c.chunk());
    assert_eq!(rule(&f), Rule::AutoConfig);
}

#[test]
fn boundary_spanning_presentation_is_charged_to_neither_frame() {
    // Review finding 2: a wait that starts in frame A and ends in frame B belongs to neither.  The writer counts it as
    // spanning in both rows and charges neither; the reader names the attribution unavailable unless the frame has
    // complete flip-wait scopes of its own reaching the share.  Nothing is clipped into an invented per-frame wait.
    let none = Blk::default();
    let span = Blk { spanning: 1, ..none };
    let scope_span = Blk { scope_spanning: 1, ..none };
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0); 4], [none, scope_span, none, none]).bytes()).unwrap());
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    assert!(r.frames[1].reason.ends_with("a flip-wait scope spans a frame boundary"), "{}", r.frames[1].reason);
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0); 4], [none, span, none, none]).bytes()).unwrap());
    assert!(r.frames[1].reason.ends_with("flip-status polling spans a frame boundary"), "{}", r.frames[1].reason);
    // spanning cadence next to complete cadence: still unavailable, cadence never establishes a wait anyway
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0); 4], [none, Blk { runs: 3, ticks: 90_000, ..span },
        none, none]).bytes()).unwrap());
    assert!(r.frames[1].reason.ends_with("flip-status polling spans a frame boundary"), "{}", r.frames[1].reason);
    // a complete scope of its own that reaches the share keeps PACING beside a spanning one
    let r = analyze(&reader::read(&four_frames_scopes([(0, 0), (1, 50_000), (0, 0), (0, 0)], [none, scope_span, none,
        none]).bytes()).unwrap());
    assert_eq!(r.frames[1].verdict, Verdict::Pacing, "{}", r.frames[1].reason);
    let p = r.frames[1].presentation.as_ref().unwrap();
    assert_eq!((p.flip_wait, p.flip_wait_spanning), (Some((50_000, 1)), 1));
}

#[test]
fn coverage_totals_are_checked_and_reconciled_by_kind() {
    // Review finding 3: each kind's row is valid on its own (visited = observed + exclusions), but the observed counts
    // summed across kinds overflow u64.  Unchecked, debug panics and release wraps to 1, so one record passed the
    // count gate.  Checked: a named UNKNOWN.
    let mut f = four_frames();
    let at = f.chunks.iter().enumerate().filter(|(_, c)| c.ty == 36).nth(1).map(|(i, _)| i).unwrap();
    let mut x = row(1, (110_000, 210_000), 3);
    x.rows[0][0] = u64::MAX - 1;
    x.rows[0][1] = u64::MAX - 1;
    f.chunks[at] = coverage(&x);
    let r = analyze(&reader::read(&f.bytes()).expect("each kind reconciles on its own"));
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    assert_eq!(r.frames[1].reason, "coverage totals overflow");
    // equal totals, wrong kind: the row observed one ARRAY, the retained record is a CLEAR
    let mut f = four_frames();
    let mut x = row(1, (110_000, 210_000), 0);
    x.rows[0][0] = 1;
    x.rows[0][1] = 1;
    f.chunks[at] = coverage(&x);
    let r = analyze(&reader::read(&f.bytes()).unwrap());
    assert_eq!(r.frames[1].verdict, Verdict::Unknown);
    assert_eq!(r.frames[1].reason, "0 ARRAY operation record(s) for 1 observed");
    assert_eq!(r.frames[1].observed, [1, 0, 0]);
}

// ---- build-frame writer (CONFIG masks 63/11): segment records name their building frame -----------------------------

fn bld_cfg() -> ACfg {
    let mut c = pres_cfg();
    c.words[4] = 63;
    c.words[13] = 144;
    c
}
fn bld_file(rest: Vec<Chunk>) -> File {
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(bld_cfg().chunk());
    f.chunks.extend(rest);
    f
}
fn ops_b(list: &[AOp]) -> Chunk {
    ops_w(list, 144)
}
/// A 384-byte 63/11 row with `unrepresented` at u32@380.
fn coverage_b(r: &ARow, unrepresented: u32) -> Chunk {
    coverage_p(r, &Blk { reserved: unrepresented, ..Blk::default() })
}
/// A segment record recorded under `frame` (call ordinal `call`), built in `built`.
fn built_op(id: u64, frame: u64, raw: (u64, u64), segment: u32, call: u32, built: u64) -> AOp {
    let s = 2 * id as u32 - 1;
    let mut o = seg_op(id, frame, raw, segment, call, (s, s + 1));
    o.tail = (built, OWNER, 1, 0);
    o
}
/// A dropped build (never called, or its CALL refused): INCOMPLETE, no CALL token, call ordinal 0.
fn dropped_op(id: u64, segment: u32, built: u64) -> AOp {
    let mut o = built_op(id, 0, (0, 0), segment, 0, built);
    o.flags = 64 | 4 | 16;
    o.owner = 0;
    o.frame = 0;
    o.published = 0;
    o
}
/// four_frames re-encoded for 63/11: the given OPS records and rows (frame i at tb 10000 + 100000 i, `clears[i]`
/// observed clears, `unrepresented[i]`); FIFO waits and anchors unchanged.
fn four_frames_built(list: &[AOp], clears: [u64; 4], unrepresented: [u32; 4], status_flags: u16) -> File {
    let mut f = four_frames();
    let at = f.chunks.iter().position(|c| c.ty == 34).unwrap();
    f.chunks[at] = bld_cfg().chunk();
    let at = f.chunks.iter().position(|c| c.ty == 35).unwrap();
    f.chunks[at] = ops_b(list);
    let rows: Vec<usize> = f.chunks.iter().enumerate().filter(|(_, c)| c.ty == 36).map(|(i, _)| i).collect();
    for (k, (&i, t)) in rows.iter().zip([10_000u64, 110_000, 210_000, 310_000]).enumerate() {
        f.chunks[i] = coverage_b(&row(k as u64, (t, t + 100_000), clears[k]), unrepresented[k]);
    }
    let published = list.iter().map(|o| o.published).max().unwrap_or(0);
    let last = f.chunks.len() - 1;
    let mut n = status_for(list.len() as u64, 4, 9, published);
    n[0] = list.iter().map(|o| o.id).max().unwrap_or(0);
    let held = list.iter().filter(|o| o.flags & 16 != 0).count() as u64;
    n[14] = held;
    f.chunks[last] = status(status_flags | if held != 0 { 2 | 64 } else { 0 }, n);
    f
}
fn verdicts(r: &Report) -> Vec<Verdict> {
    r.frames.iter().map(|f| f.verdict).collect()
}

#[test]
fn build_frames_cross_frame_fixture() {
    // The review's cross-frame fixture on the build-frame writer: A built in frame 0 (one observed clear, one excluded for room) and
    // called in frame 1 while B is built there; B called in frame 2.  Frame 1 imports A from an unclean build row:
    // UNKNOWN naming frame 0.  Frame 2 imports B, whose build row (1) is clean and reconciled, so it qualifies even
    // though frame 1's own verdict is UNKNOWN: the source predicate does not recurse through verdicts.
    let list = [built_op(1, 1, (60_000, 61_000), 1, 1, 0), built_op(2, 2, (110_000, 140_000), 2, 2, 1)];
    let mut f0 = row(0, (10_000, 110_000), 1);
    f0.rows[2][0] = 2;
    f0.rows[2][2] = 1;
    let rest = vec![events(&frame_wrap(1, 1, 220_000, 20_000, 10_000, 2)), St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(),
        ops_b(&list), coverage_b(&f0, 0), coverage_b(&row(1, (110_000, 210_000), 1), 0),
        coverage_b(&row(2, (210_000, 310_000), 0), 0)];
    let mut rest = rest;
    rest.extend(anchors().iter().map(clock));
    rest.push(status(1, status_for(2, 3, 9, 4)));
    let r = analyze(&reader::read(&bld_file(rest).bytes()).expect("reads"));
    assert!(r.build_frames);
    let (f0, f1, f2) = (&r.frames[0], &r.frames[1], &r.frames[2]);
    assert!(f0.reason.contains("excluded"), "{}", f0.reason);
    assert_eq!(f1.verdict, Verdict::Unknown);
    assert_eq!(f1.reason, format!("segment draw(s) built in frame {OWNER}/0: 1 covered operation(s) excluded"));
    assert_eq!((f1.records, f1.recorded, f1.segment_imports), ([0, 0, 1], 1, 1));
    assert_eq!(f2.verdict, Verdict::RsxBound, "{}", f2.reason);
    assert_eq!((f2.records, f2.recorded, f2.segment_imports, f2.observed), ([0; 3], 1, 1, [0; 3]));
    // the CALL-only frame (observed 0) is timed by what it executed, not forced to an empty span
    let (lo, _) = f2.rsx_span_ppu.expect("mapped span");
    assert!(lo >= 50_000, "{lo}");
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_json(&mut out, &r).unwrap();
    let j: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert_eq!((j["frames"][2]["built"].as_u64(), j["frames"][2]["recorded"].as_u64()), (Some(0), Some(1)));
    assert_eq!(j["frames"][1]["segment_imports"], 1);
    assert_eq!(j["segment_replay"], false);
}

#[test]
fn clean_double_buffer_gets_verdicts() {
    // Frame N builds part N and calls part N-1 (frame 0 draws in the ring; frame 3 only calls).  Every build row is
    // clean, so every frame keeps the verdict its own evidence gives (the four_frames verdicts).
    let mut ring = op(1, 0, (6000, 36_000));
    ring.serials = (1, 2);
    ring.published = 2;
    let list = [ring, built_op(2, 1, (60_000, 61_000), 1, 1, 0), built_op(3, 2, (110_000, 150_000), 2, 2, 1),
        built_op(4, 3, (160_000, 200_000), 3, 3, 2)];
    let r = analyze(&reader::read(&four_frames_built(&list, [2, 1, 1, 0], [0; 4], 1).bytes()).expect("reads"));
    assert_eq!(verdicts(&r), [Verdict::RsxBound, Verdict::Unknown, Verdict::Mixed, Verdict::Unknown], "{:#?}", r.frames);
    assert_eq!(r.frames[1].reason, ps3dk_prof::auto_rsx::NOT_DISTINGUISHED);
    assert!(r.frames[3].reason.starts_with("unresolved"), "{}", r.frames[3].reason);
    assert_eq!(r.frames.iter().map(|f| f.segment_imports).collect::<Vec<_>>(), [0, 1, 1, 1]);
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_text(&mut out, &r).unwrap();
    let text = String::from_utf8(out).unwrap();
    assert!(text.contains("segments: built records [0, 0, 2] recorded here 1; imports 0"), "{text}");
}

#[test]
fn build_only_frame_is_an_empty_execution() {
    // Frame 1 builds part 1 and executes nothing (its CALL is in frame 2): its span is the empty-execution case, not
    // a mismatch between its one observed clear and zero records under it.
    let mut ring = op(1, 0, (6000, 36_000));
    ring.serials = (1, 2);
    ring.published = 2;
    let list = [ring, built_op(2, 2, (110_000, 150_000), 1, 1, 1)];
    let r = analyze(&reader::read(&four_frames_built(&list, [1, 1, 0, 0], [0; 4], 1).bytes()).expect("reads"));
    let f1 = &r.frames[1];
    assert_eq!((f1.recorded, f1.records, f1.observed), (0, [0, 0, 1], [0, 0, 1]));
    assert_eq!(f1.rsx_span_raw, None);
    assert_eq!(f1.reason, ps3dk_prof::auto_rsx::NOT_DISTINGUISHED);
    assert_eq!(r.frames[2].verdict, Verdict::Mixed, "{}", r.frames[2].reason);
}

#[test]
fn unrepresented_calls_refuse_an_otherwise_clean_frame() {
    // A re-CALL, an all-excluded build, an untracked part or a refused CALL leave no record but count in the row;
    // frame 0 also has a valid ring draw, so an empty timing population cannot be what refuses it.
    let mut ring = op(1, 0, (6000, 36_000));
    ring.serials = (1, 2);
    ring.published = 2;
    for (n, want) in [(1u32, "at least 1 segment draw(s)"), (u32::MAX, "at least 4294967295 segment draw(s)")] {
        let r = analyze(&reader::read(&four_frames_built(&[ring], [1, 0, 0, 0], [n, 0, 0, 0], 1).bytes()).unwrap());
        assert_eq!(r.frames[0].verdict, Verdict::Unknown);
        assert!(r.frames[0].reason.starts_with(want), "{}", r.frames[0].reason);
        assert_eq!(r.frames[0].segment_unrepresented, n);
    }
    let r = analyze(&reader::read(&four_frames_built(&[ring], [1, 0, 0, 0], [0; 4], 1).bytes()).unwrap());
    assert_eq!(r.frames[0].verdict, Verdict::RsxBound, "{}", r.frames[0].reason);
}

#[test]
fn dropped_builds_and_lost_records() {
    // Frame 1 builds S (dropped: never called) and T (called in frame 2).  S's INCOMPLETE record keeps frame 1's
    // population whole: frame 1 is UNKNOWN (never called), frame 2's import of T from frame 1 is unaffected.
    let mut ring = op(1, 0, (6000, 36_000));
    ring.serials = (1, 2);
    ring.published = 2;
    let list = [ring, dropped_op(2, 1, 1), built_op(3, 2, (110_000, 150_000), 2, 1, 1)];
    let r = analyze(&reader::read(&four_frames_built(&list, [1, 2, 0, 0], [0; 4], 1).bytes()).expect("reads"));
    assert_eq!(r.frames[1].segment_abandoned, 1);
    assert_eq!(r.frames[1].reason, "1 segment draw(s) built in this frame were dropped (never called, or their CALL \
        refused)");
    assert_eq!(r.frames[2].verdict, Verdict::Mixed, "{}", r.frames[2].reason);
    // the same with S's record lost (metadata full): frame 1's population is short, and so is every import from it
    let list = [ring, built_op(3, 2, (110_000, 150_000), 2, 1, 1)];
    let r = analyze(&reader::read(&four_frames_built(&list, [1, 2, 0, 0], [0; 4], 1).bytes()).expect("reads"));
    assert_eq!(r.frames[1].reason, "1 CLEAR operation record(s) for 2 observed");
    assert_eq!(r.frames[2].verdict, Verdict::Unknown);
    assert_eq!(r.frames[2].reason, format!("segment draw(s) built in frame {OWNER}/1: 1 CLEAR operation record(s) for \
        2 observed"));
    // a segment record built while no frame was open has no build source
    let mut o = built_op(3, 2, (110_000, 150_000), 2, 1, 0);
    o.tail = (0, 0, 0, 0);
    let r = analyze(&reader::read(&four_frames_built(&[ring, o], [1, 0, 0, 0], [0; 4], 1).bytes()).expect("reads"));
    assert_eq!(r.frames[2].reason, "segment record(s) built while no coverage frame was open");
}

#[test]
fn segment_replay_makes_every_frame_unknown() {
    let mut ring = op(1, 0, (6000, 36_000));
    ring.serials = (1, 2);
    ring.published = 2;
    let r = analyze(&reader::read(&four_frames_built(&[ring], [1, 0, 0, 0], [0; 4], 1 | 1024).bytes()).unwrap());
    assert!(r.frames.iter().all(|f| f.verdict == Verdict::Unknown && f.reason == ps3dk_prof::auto_rsx::SEGMENT_REPLAY),
        "{:#?}", r.frames);
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_text(&mut out, &r).unwrap();
    assert!(String::from_utf8(out).unwrap().contains("SEGMENT REPLAY:"));
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_json(&mut out, &r).unwrap();
    let j: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert_eq!(j["segment_replay"], true);
    // sticky across statuses, and unknown to every older mask pair
    let e = reader::read(&bld_file(vec![status(1024, [0; 15]), status(1, [0; 15])]).bytes()).unwrap_err();
    assert_eq!(e.rule, Rule::AutoStatus);
    assert!(e.detail.contains("sticky"), "{}", e.detail);
    assert!(reader::read(&bld_file(vec![status(1 | 1024, [0; 15])]).bytes()).is_ok());
    assert_eq!(rule(&pres_file(vec![status(1 | 1024, [0; 15])])), Rule::AutoStatus);
    assert_eq!(rule(&seg_file(vec![status(1 | 1024, [0; 15])])), Rule::AutoStatus);
}

#[test]
fn build_tail_and_width_refusals() {
    let ok = built_op(1, 0, (1, 2), 1, 1, 0);
    // known build frame 0 is valid; a dropped record carries a build token without a CALL token
    assert!(reader::read(&bld_file(vec![ops_b(&[ok])]).bytes()).is_ok());
    assert!(reader::read(&bld_file(vec![ops_b(&[dropped_op(1, 1, 5)])]).bytes()).is_ok());
    let cases: Vec<(AOp, &str)> = vec![
        ({ let mut o = ok; o.tail.2 = 3; o }, "build tail flags"),
        ({ let mut o = ok; o.tail.3 = 1; o }, "build tail flags"),
        ({ let mut o = ok; o.tail.1 = OWNER + 1; o }, "build owner"),
        ({ let mut o = ok; o.tail = (0, OWNER, 0, 0); o }, "without BUILD_KNOWN"),
        ({ let mut o = ok; o.tail = (4, 0, 0, 0); o }, "without BUILD_KNOWN"),
        ({ let mut o = op(1, 0, (1, 2)); o.tail = (0, OWNER, 1, 0); o }, "not built in a segment"),
        ({ let mut o = op(1, 0, (1, 2)); o.tail = (3, 0, 0, 0); o }, "not built in a segment"),
        (built_op(1, 2, (1, 2), 1, 1, 3), "after its CALL frame"),
    ];
    for (o, want) in cases {
        let e = reader::read(&bld_file(vec![ops_b(&[o])]).bytes()).expect_err(want);
        assert_eq!(e.rule, Rule::AutoOps, "{want}");
        assert!(e.detail.contains(want), "{want}: {}", e.detail);
    }
    // the width is negotiated with the masks: 144 only with 63/11, on CONFIG and on every OPS chunk
    assert_eq!(rule(&bld_file(vec![ops(&[op(1, 0, (1, 2))])])), Rule::AutoOps);
    assert_eq!(rule(&pres_file(vec![ops_b(&[op(1, 0, (1, 2))])])), Rule::AutoOps);
    let mut c = bld_cfg();
    c.words[13] = 128;
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(c.chunk());
    assert_eq!(rule(&f), Rule::AutoConfig);
    let mut c = pres_cfg();
    c.words[13] = 144;
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(c.chunk());
    assert_eq!(rule(&f), Rule::AutoConfig);
    // u32@380 carries the unrepresented count only with 63/11; older rows keep it reserved zero
    let mut x = row(0, (10, 20), 0);
    x.flags |= 64;
    assert!(reader::read(&bld_file(vec![coverage_b(&x, 7)]).bytes()).is_ok());
    let e = reader::read(&pres_file(vec![coverage_b(&x, 7)]).bytes()).unwrap_err();
    assert!(e.detail.contains("reserved"), "{}", e.detail);
}

#[test]
fn build_frame_fixture_from_the_host_runtime() {
    // The segbuild host control (auto-segment-build-frame-test.c): the writer's 63/11 bytes read back by this reader.
    let r = read_fixture("segbuild");
    let Some(Config::Attached(a)) = &r.config else { panic!("attached") };
    assert_eq!((a.kind_mask, a.form_mask), (63, 11));
    assert!(r.build_frames);
    assert!(r.status.as_ref().is_some_and(|s| s.flags & ps3dk_prof::auto_rsx::ST_SEGMENT_REPLAY != 0));
    assert_eq!(r.frames.iter().map(|f| f.segment_unrepresented).collect::<Vec<_>>(), [0, 3, u32::MAX]);
    // frame 1 executed A (built in 0) and E (one draw built in 0, one in 1); frame 2 built R, whose CALL was refused
    assert_eq!((r.frames[1].recorded, r.frames[1].segment_imports), (3, 2));
    assert_eq!(r.frames[2].segment_abandoned, 1);
    // a segment was replayed: no frame of this capture has a verdict
    assert!(r.frames.iter().all(|f| f.verdict == Verdict::Unknown && f.reason == ps3dk_prof::auto_rsx::SEGMENT_REPLAY),
        "{:#?}", r.frames);
}

// ---- bracketed clock anchors (CONFIG masks 127/11) ------------------------------------------------------------------

fn brk_cfg() -> ACfg {
    let mut c = bld_cfg();
    c.words[4] = 127;
    c
}
fn brk_file(cfg: ACfg, rest: Vec<Chunk>) -> File {
    let mut f = base(AUTO_FLAGS);
    f.chunks.push(cfg.chunk());
    f.chunks.extend(rest);
    f
}
/// Anchor k one second of RSX time after k-1 (raw ns), PPU time at 79.8 MHz, bracketed by `half` ticks either side,
/// with the bracketed label rule (seen below 2k-1, then at or above 2k).
fn brk_anchor(id: u32, half: u64) -> AClk {
    let raw = 1_000_000_000u64 * u64::from(id);
    let t = raw / 1000 * 798 / 10 + 5_000_000; // 79.8 M ticks per second of RSX time
    let mut a = anchor(id, raw, (t - half, t + half));
    a.labels = (0, 2 * id);
    a
}
fn timed_out(id: u32) -> AClk {
    let mut a = brk_anchor(id, 1000);
    a.flags = 2;
    a.raw = 0;
    a.repeated = 0;
    a.labels = (0, 0);
    a
}

/// 10 ms brackets (the drain interval) around nine one-second-apart anchors still qualify the map.
#[test]
fn bracketed_anchors_qualify_with_drain_interval_brackets() {
    let half = FREQ / 200; // +/- 5 ms
    let mut rest: Vec<Chunk> = (1..=9).map(|k| clock(&brk_anchor(k, half))).collect();
    rest.push(status(1, status_for(0, 0, 9, 0)));
    let r = analyze(&reader::read(&brk_file(brk_cfg(), rest).bytes()).expect("reads"));
    let Some(MapState::Qualified(m)) = &r.map else { panic!("map {:?}", r.map) };
    assert_eq!(m.anchors, 9);
}

/// Under 127/11 a timed-out anchor removes an observation but does not unqualify the map; under 63/11 it still does.
#[test]
fn bracketed_map_ignores_failed_anchors() {
    let half = FREQ / 200;
    let ids = [1u32, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
    let build = |cfg: ACfg, bracket: bool| {
        let mut rest: Vec<Chunk> = ids.iter().map(|&k| {
            let mut a = if k == 4 || k == 8 { timed_out(k) } else { brk_anchor(k, half) };
            if !bracket && a.flags == 1 { a.labels = (k, k); }
            clock(&a)
        }).collect();
        rest.push(status(1, status_for(0, 0, 11, 0)));
        analyze(&reader::read(&brk_file(cfg, rest).bytes()).expect("reads"))
    };
    let r = build(brk_cfg(), true);
    let Some(MapState::Qualified(m)) = &r.map else { panic!("map {:?}", r.map) };
    assert_eq!(m.anchors, 9, "two timed-out anchors left out");
    let r = build(bld_cfg(), false);
    assert!(matches!(&r.map, Some(MapState::Unqualified(why)) if why.contains("anchor 4 failed")), "{:?}", r.map);
}

/// The label rule belongs to the CONFIG: 127/11 needs (below 2k-1, at or above 2k), older masks need (k, k).
#[test]
fn bracketed_label_rule_follows_the_config() {
    let mut a = brk_anchor(1, 1000);
    a.labels = (1, 1);
    let f = brk_file(brk_cfg(), vec![clock(&a)]);
    assert!(reader::read(&f.bytes()).is_err(), "127/11 refuses the unbracketed label pair");
    let a = brk_anchor(1, 1000); // labels (0, 2)
    let f = brk_file(bld_cfg(), vec![clock(&a)]);
    assert!(reader::read(&f.bytes()).is_err(), "63/11 refuses the bracketed label pair");
    let mut a = brk_anchor(2, 1000);
    a.labels = (3, 4); // seen at 2k-1: not a lower bound
    let f = brk_file(brk_cfg(), vec![clock(&brk_anchor(1, 1000)), clock(&a)]);
    assert!(reader::read(&f.bytes()).is_err(), "a lower-bound poll must have seen the label below 2k-1");
}

// ---- whole-CALL brackets (CONFIG masks 255/11) ----------------------------------------------------------------------

/// 255/11: one frame (PPU 10000..110000) with a FIFO wait, marks at RSX 5000..45000 and one short timed clear, plus one
/// segment draw refused for report capacity while building in this frame and CALLed here (unrepresented 1). `call`
/// is the whole-CALL bracket: (raw span, flags, args[1] without a record, args[4] build-excluded), or None.
fn call_frame(call: Option<((u64, u64), u16, u32, u32)>, unrep: u32, masks: u32) -> File {
    let mut list = vec![mark(1, 0, 1, 5000), op(2, 0, (6000, 7000))];
    if let Some((raw, flags, without, excl)) = call {
        let mut c = op(3, 0, raw);
        c.kind = 5;
        c.flags = flags;
        if flags & 4 != 0 {
            c.published = 0;
        }
        c.args = [1, without, 1, 1, excl, 0, 0];
        if excl != 0 {
            c.tail = (0, OWNER, 1, 0);
        }
        list.push(c);
    }
    let end = list.len() as u64 + 1;
    list.push(mark(end, 0, 2, 45_000));
    let n = list.len() as u64;
    let mut x = row(0, (10_000, 110_000), 1);
    x.rows[2][0] = 2;
    x.rows[2][3] = 1;
    x.flags |= 64;
    let mut rest = vec![events(&frame_wrap(1, 1, 20_000, 20_000, 10_000, 0)),
        St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(), ops_w(&list, 144), coverage_p(&x, &Blk { reserved: unrep, ..Blk::default() })];
    rest.extend(anchors().into_iter().map(|mut a| { a.labels = (0, 2 * a.id); clock(&a) }));
    rest.push(status(1, status_for(n, 1, 9, 2 * n)));
    let mut cfg = brk_cfg();
    cfg.words[4] = masks;
    brk_file(cfg, rest)
}

/// A whole-CALL bracket times every draw its CALL executed: the draws it names leave the calling frame's untimed
/// count (and its build frame's, for those refused while building there), and its span joins the frame's RSX span,
/// so a frame the timed draws alone could not decide gets the exact verdict. A failed bracket covers nothing.
#[test]
fn call_brackets_decide_untimed_segment_frames() {
    let read = |f: File| analyze(&reader::read(&f.bytes()).expect("reads")).frames[0].clone();
    // no bracket: the short timed span with an untimed draw decides nothing
    let f = read(call_frame(None, 1, 255));
    assert_eq!(f.verdict, Verdict::Unknown);
    assert_eq!(f.rsx_busy_raw, Some(1000));
    assert!(f.reason.contains("does not decide"), "{}", f.reason);
    assert_eq!(f.call_brackets, 0);
    // the bracket covers the unrepresented draw and its build-frame exclusion: the exact rule, no bounds
    let f = read(call_frame(Some(((6500, 40_000), 3, 1, 1)), 1, 255));
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.starts_with("fetch:"), "{}", f.reason);
    assert!(f.rsx_busy_bounds.is_none());
    assert_eq!(f.call_brackets, 1);
    assert_eq!(f.rsx_span_raw, Some(40_000 - 6000));
    // busy time merges the overlapping clear into the CALL's interval
    assert_eq!(f.rsx_busy_raw, Some(40_000 - 6000));
    assert!(f.rsx_busy_ppu.is_some());
    // a gap between the clear and the CALL: in the span, not in busy time
    let f = read(call_frame(Some(((8000, 40_000), 3, 1, 1)), 1, 255));
    assert_eq!((f.rsx_span_raw, f.rsx_busy_raw), (Some(34_000), Some(1000 + 32_000)));
    // a bracket that names no build-frame exclusion leaves that draw untimed: bounded, on a lower bound that now
    // includes the CALL's span
    let f = read(call_frame(Some(((6500, 40_000), 3, 1, 0)), 1, 255));
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.starts_with("1 covered draw(s) untimed; verdict holds"), "{}", f.reason);
    // an INCOMPLETE bracket covers nothing and adds no span
    let f = read(call_frame(Some(((0, 0), 1 | 4 | 16, 1, 1)), 1, 255));
    assert_eq!(f.verdict, Verdict::Unknown);
    assert!(f.reason.contains("does not decide"), "{}", f.reason);
    assert_eq!(f.call_brackets, 0);
    // a bracket naming more draws than the row counted without a record refuses
    let f = read(call_frame(Some(((6500, 40_000), 3, 1, 1)), 0, 255));
    assert_eq!(f.verdict, Verdict::Unknown);
    assert!(f.reason.contains("above the row's 0 unrepresented"), "{}", f.reason);
}

/// CALL records are a 255/11 kind with fixed argument rules; older masks refuse kind 5.
#[test]
fn call_brackets_are_checked() {
    assert!(reader::read(&call_frame(Some(((6500, 40_000), 3, 1, 1)), 1, 255).bytes()).is_ok());
    assert!(reader::read(&call_frame(Some(((6500, 40_000), 3, 1, 1)), 1, 127).bytes()).is_err(), "127/11 has no kind 5");
    // without a record > covered, build-excluded > without a record, a SEGMENT flag, no token
    for (flags, without, excl) in [(3, 2, 0), (3, 0, 1), (3 | 64, 1, 1), (2, 1, 1)] {
        let f = call_frame(Some(((6500, 40_000), flags, without, excl)), 1, 255);
        assert!(reader::read(&f.bytes()).is_err(), "flags {flags} args {without}/{excl}");
    }
    // a build tail exactly with build-excluded draws
    let mut f = call_frame(Some(((6500, 40_000), 3, 1, 0)), 1, 255);
    let at = f.chunks.iter().position(|c| c.ty == 35).unwrap();
    let base = 16 + 144 * 2; // the third record
    f.chunks[at].payload[base + 140] = 1;
    f.chunks[at].payload[base + 136..base + 140].copy_from_slice(&OWNER.to_le_bytes());
    assert!(reader::read(&f.bytes()).is_err(), "a tail without build-excluded draws");
}

// ---- busy-time polling rule and label-poll cadence (CONFIG masks 255/11, 511/11) -----------------------------------

/// One frame (PPU 10000..110000) with frame marks at RSX 5000..45000, one timed clear per `clears` interval (raw), no
/// FIFO wait, no flip-wait scope, the flip-status cadence block `flip` and (511/11) the label-poll block `label`
/// (its one-label total is the whole cadence for one label; `busy_frame_labels` names it).
fn busy_frame(clears: &[(u64, u64)], flip: Blk, label: Option<(Blk, u32)>, masks: u32) -> File {
    busy_frame_labels(clears, flip, label.map(|(b, l)| (b, l, b.ticks)), masks)
}
fn busy_frame_labels(clears: &[(u64, u64)], flip: Blk, label: Option<(Blk, u32, u64)>, masks: u32) -> File {
    let mut list = vec![mark(1, 0, 1, 5000)];
    for (i, &c) in clears.iter().enumerate() {
        list.push(op(i as u64 + 2, 0, c));
    }
    let end = list.len() as u64 + 1;
    list.push(mark(end, 0, 2, 45_000));
    let n = list.len() as u64;
    let mut x = row(0, (10_000, 110_000), clears.len() as u64);
    x.flags |= 64;
    let mut cov = coverage_p(&x, &flip);
    if masks & 256 != 0 {
        let (l, index, one) = label.unwrap_or_default();
        let mut b = Buf::default();
        b.u32(l.runs).u32(l.broken).u32(l.spanning).u32(index).u64(l.ticks).u64(one);
        cov.payload.extend_from_slice(&b.0);
    }
    if masks & 512 != 0 {
        cov.payload.extend_from_slice(&[0; 64]); // timed-range block and flip-wait scope block: empty
    }
    let width = if masks & 512 != 0 { 160 } else { 144 };
    let mut rest = vec![events(&frame_wrap(1, 1, 20_000, 20_000, 10_000, 7)),
        St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk(), ops_w(&list, width), cov];
    rest.extend(anchors().into_iter().map(|mut a| { a.labels = (0, 2 * a.id); clock(&a) }));
    rest.push(status(1, status_for(n, 1, 9, 2 * n)));
    let mut cfg = brk_cfg();
    cfg.words[4] = masks;
    if masks & 512 != 0 {
        cfg.words[13] = 160;
    }
    brk_file(cfg, rest)
}
fn busy_read(f: File) -> ps3dk_prof::auto_rsx::FrameView {
    analyze(&reader::read(&f.bytes()).expect("reads")).frames[0].clone()
}
fn polls(ticks: u64) -> Blk {
    Blk { runs: 1, ticks, ..Blk::default() }
}

/// RSX-bound from polling needs RSX BUSY time (merged timed intervals) of at least half the frame together with
/// complete polling runs of at least a quarter and no FIFO wait. A long span with short busy time stays UNKNOWN and
/// prints the facts as an observation.
#[test]
fn busy_polling_needs_busy_time() {
    // the review counterexample: two 1-tick clears 30000 raw ticks apart, cadence 30000 of 100000 PPU ticks
    let f = busy_read(busy_frame(&[(6000, 6001), (36_000, 36_001)], polls(30_000), None, 255));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    assert!(f.reason.contains("observation, not a verdict"), "{}", f.reason);
    assert!(!f.reason.contains("presented"), "{}", f.reason);
    assert_eq!(f.rsx_busy_raw, Some(2));
    // pacing or queue gaps: two short clears far apart, the RSX idle between them
    let f = busy_read(busy_frame(&[(6000, 7000), (36_000, 37_000)], polls(30_000), None, 255));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    assert!(f.reason.contains("observation, not a verdict"), "{}", f.reason);
    // useful CPU work between pending polls: every run broken, nothing complete
    let worked = Blk { broken: 6, ..Blk::default() };
    let f = busy_read(busy_frame(&[(6000, 36_000)], worked, None, 255));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    // busy >= 1/2 and complete flip-status polling >= 1/4: RSX-bound, named by busy time and polling
    let f = busy_read(busy_frame(&[(6000, 36_000)], polls(30_000), None, 255));
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.starts_with("busy polling: RSX busy time >= "), "{}", f.reason);
    assert!(f.reason.contains("polled flip status for >= 30000 ticks"), "{}", f.reason);
    // polling below a quarter (0.24 s of a 1.93 s frame is 12.4%): UNKNOWN
    let f = busy_read(busy_frame(&[(6000, 36_000)], polls(12_400), None, 255));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    // a run across the frame boundary charges no frame
    let f = busy_read(busy_frame(&[(6000, 36_000)], Blk { spanning: 1, ..polls(30_000) }, None, 255));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    // older masks keep their rule and text: no busy rule, no observation
    let f = busy_read(busy_frame(&[(6000, 36_000)], polls(30_000), None, 127));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    assert!(f.reason.starts_with("unresolved: FIFO wait >= 0") && !f.reason.contains("observation"), "{}", f.reason);
}

/// 511/11: label-poll cadence alone reaches the polling share; its block is checked.
#[test]
fn label_polling_counts_with_busy_time() {
    let label = Some((polls(30_000), 70 + 1));
    let f = busy_read(busy_frame(&[(6000, 36_000)], Blk::default(), label, 511));
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.contains("polled a label for >= 30000 ticks"), "{}", f.reason);
    assert_eq!(f.label_polls.map(|(c, l, one)| (c.runs, c.ticks, l, one)), Some((1, 30_000, 71, 30_000)));
    // the same label polling with short busy time: UNKNOWN with the observation
    let f = busy_read(busy_frame(&[(6000, 6001), (36_000, 36_001)], Blk::default(), label, 511));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    assert!(f.reason.contains("30000 (labels)"), "{}", f.reason);
    // malformed blocks: a label without runs, runs without a label, a label above 256
    for (b, l) in [(Blk::default(), 5), (polls(10), 0), (polls(10), 300)] {
        assert!(reader::read(&busy_frame(&[(6000, 36_000)], Blk::default(), Some((b, l)), 511).bytes()).is_err(), "{l}");
    }
    // the one-label total: equal to the cadence for one label; for several, at least two runs and at most the sum
    let two = Blk { runs: 2, ticks: 30_000, ..Blk::default() };
    for (b, l, one) in [(polls(30_000), 71, 29_999), (polls(30_000), 71, 30_001), (two, u32::MAX, 30_001),
                        (polls(30_000), u32::MAX, 30_000)] {
        let f = busy_frame_labels(&[(6000, 36_000)], Blk::default(), Some((b, l, one)), 511);
        assert!(reader::read(&f.bytes()).is_err(), "{l:#x} {one}");
    }
    // 255/11 rows are 384 bytes: a 416-byte row is refused, and the reverse
    let mut f = busy_frame(&[(6000, 36_000)], Blk::default(), None, 255);
    let at = f.chunks.iter().position(|c| c.ty == 36).unwrap();
    f.chunks[at].payload.extend_from_slice(&[0; 32]);
    assert!(reader::read(&f.bytes()).is_err());
}

/// The review counterexample: two complete runs on DIFFERENT labels, 15000 ticks each in a 100000-tick frame, with
/// RSX busy time over half the frame and no FIFO wait.  No one label reaches the quarter-frame share, so the frame is not
/// RSX-bound; the one-label total decides, never the sum across labels.
#[test]
fn label_cadence_is_never_summed_across_labels() {
    let two = Blk { runs: 2, ticks: 30_000, ..Blk::default() };
    let f = busy_read(busy_frame_labels(&[(6000, 36_000)], Blk::default(), Some((two, u32::MAX, 15_000)), 511));
    assert_ne!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.contains("15000 (labels)"), "{}", f.reason);
    assert_eq!(f.label_polls.map(|(c, l, one)| (c.ticks, l, one)), Some((30_000, u32::MAX, 15_000)));
    // several labels where ONE of them meets the share on its own: RSX-bound, named by that label's total
    let f = busy_read(busy_frame_labels(&[(6000, 36_000)], Blk::default(), Some((two, u32::MAX, 25_000)), 511));
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.contains("polled a label for >= 25000 ticks"), "{}", f.reason);
}

/// 255/11 and later report each frame's polling shares in auto --json.
#[test]
fn polling_shares_in_json() {
    let r = analyze(&reader::read(&busy_frame(&[(6000, 36_000)], polls(12_400), Some((polls(25_000), 71)), 511).bytes())
        .expect("reads"));
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_json(&mut out, &r).unwrap();
    let v: serde_json::Value = serde_json::from_slice(&out).unwrap();
    let fr = &v["frames"][0];
    assert_eq!(fr["cadence_share"].as_f64(), Some(0.124), "{fr}");
    assert_eq!(fr["label_cadence_share"].as_f64(), Some(0.25), "{fr}");
    assert_eq!(fr["label_polling"]["label"], 70, "{fr}");
    assert_eq!(fr["label_polling"]["one_label_ticks"], 25_000, "{fr}");
    // older masks: no shares
    let r = analyze(&reader::read(&busy_frame(&[(6000, 36_000)], polls(12_400), None, 127).bytes()).expect("reads"));
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_json(&mut out, &r).unwrap();
    let v: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert!(v["frames"][0].get("cadence_share").is_none());
}

// ---- tags, AUTO ranges and timed-range limits (CONFIG masks 1023/11) ---------------------------------------------

fn cli(args: &[&str]) -> (i32, Vec<u8>) {
    let o = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(args).output().unwrap();
    (o.status.code().unwrap_or(-1), o.stdout)
}
fn fixture_path(name: &str) -> String {
    format!("{}/tests/fixtures/auto/{name}.prof", env!("CARGO_MANIFEST_DIR"))
}

/// The host runtime's tags capture (auto-runtime-test.py --case tags): per frame and per tag, records, merged busy
/// time and the draws its CALL brackets ran untimed; per range name, the ranges and their exact raw time.
#[test]
fn by_tag_on_the_runtime_capture() {
    let r = reader::read(&fixture("tags")).expect("reads");
    let b = ps3dk_prof::auto_rsx::by_tag(&r);
    assert!(b.tagged && b.frames.len() == 2);
    let f0 = &b.frames[0];
    let row = |t: u32| f0.tags.get(&t).cloned().unwrap_or_default();
    // tag 0xa: one ring clear and part 0's two draws (built under 0xa, recorded at the CALL); every interval 10 ticks
    assert_eq!((row(0xa).ops, row(0xa).busy_raw), (3, 30));
    assert_eq!((row(0xb).ops, row(0xb).busy_raw), (1, 10));
    // tag 0xc: no draw of its own, the whole-CALL bracket of part 0 (its refused draw)
    assert_eq!((row(0xc).ops, row(0xc).busy_raw, row(0xc).untimed_in_calls), (0, 10, 1));
    assert_eq!((f0.untimed, f0.unrepresented), (1, 1));
    assert_eq!(f0.ranges.get("pass").map(|r| (r.count, r.raw)), Some((4, 40)));
    assert_eq!(f0.ranges.get("outer").map(|r| r.count), Some(1));
    let f1 = &b.frames[1];
    assert_eq!(f1.untimed, 3, "three draws outside the timed ranges");
    let s = ps3dk_prof::auto_rsx::range_summary(&b);
    assert_eq!(s.get("pass"), Some(&(2, 13, 130, 65, 90)));
    let t = ps3dk_prof::auto_rsx::tag_summary(&b);
    assert_eq!(t.get(&0xe), Some(&(1, 3, 40, 40, 40)));
    let (code, out) = cli(&["auto", "--by-tag", "--json", &fixture_path("tags")]);
    assert_eq!(code, 0);
    let v: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert_eq!(v["tagged"], true);
    assert_eq!(v["summary"]["ranges"].as_array().unwrap().len(), 3);
    // an older capture has no tags: a named refusal, exit 1
    let (code, out) = cli(&["auto", "--by-tag", &fixture_path("segments")]);
    assert_eq!(code, 1);
    assert!(String::from_utf8_lossy(&out).starts_with("no tags"));
}

/// `auto --ops` names tag and fp_offset (and RANGE depth) only for 1023/11 records; `auto --json` names the new
/// exclusion reason and the refused ranges only for 1023/11 rows.
#[test]
fn tags_in_ops_and_json() {
    let (code, out) = cli(&["auto", "--ops", &fixture_path("tags")]);
    assert_eq!(code, 0);
    let text = String::from_utf8_lossy(&out).to_string();
    assert!(text.lines().nth(1).unwrap().ends_with("args tag fp_offset"), "{text}");
    assert!(text.lines().any(|l| l.contains(" RANGE ")), "{text}");
    let (_, out) = cli(&["auto", "--ops", "--json", &fixture_path("tags")]);
    let v: serde_json::Value = serde_json::from_slice(&out).unwrap();
    let ops = v["ops"].as_array().unwrap();
    assert!(ops.iter().any(|o| o["kind"] == "CALL" && o["tag"] == 0xc && o["fp_offset"] == 0x1000));
    assert!(ops.iter().any(|o| o["kind"] == "RANGE" && o["depth"] == 4));
    let (_, out) = cli(&["auto", "--ops", &fixture_path("segments")]);
    assert!(!String::from_utf8_lossy(&out).contains("fp_offset"));
    let (_, out) = cli(&["auto", "--ops", "--json", &fixture_path("segments")]);
    let v: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert!(v["ops"].as_array().unwrap().iter().all(|o| o.get("tag").is_none()));
    let (_, out) = cli(&["auto", "--json", &fixture_path("tags")]);
    let v: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert_eq!(v["frames"][1]["outside_timed_range"]["clear"], 3);
    assert_eq!(v["frames"][1]["ranges_refused"], 10);
    assert_eq!(v["frames"][0]["ranges_refused"], 3);
}

/// One busy_frame (1023/11) with `outside` more CLEAR visits left untimed outside the timed ranges.
fn outside_frame(outside: u64, visits_too: bool) -> File {
    let mut f = busy_frame(&[(6000, 36_000)], Blk { runs: 1, ticks: 30_000, ..Blk::default() }, None, 1023);
    let at = f.chunks.iter().position(|c| c.ty == 36).unwrap();
    let row = &mut f.chunks[at].payload;
    if visits_too {
        let v = u64::from_le_bytes(row[64 + 2 * 96..64 + 2 * 96 + 8].try_into().unwrap()) + outside;
        row[64 + 2 * 96..64 + 2 * 96 + 8].copy_from_slice(&v.to_le_bytes());
    }
    row[432..440].copy_from_slice(&outside.to_le_bytes());
    f
}

/// Draws left untimed outside the timed ranges are untimed for the bounded rule like the other untimed draws; the
/// row must still reconcile (visited = observed + exclusions + outside).
#[test]
fn outside_timed_draws_are_untimed() {
    let f = busy_read(outside_frame(0, true));
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.starts_with("busy polling"), "{}", f.reason);
    let f = busy_read(outside_frame(2, true));
    assert_eq!(f.outside_timed, [0, 0, 2]);
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.starts_with("2 covered draw(s) untimed; verdict holds"), "{}", f.reason);
    assert!(reader::read(&outside_frame(2, false).bytes()).is_err(), "outside visits must be counted as visits");
}

/// RANGE records are a 1023/11 kind with fixed rules; tags are only read from 160-byte records.
#[test]
fn range_records_are_checked() {
    let base = reader::read(&fixture("tags")).expect("reads");
    let inv = base.auto.as_ref().unwrap();
    let ranges: Vec<_> = inv.ops.iter().filter(|o| o.kind == 6).collect();
    assert_eq!(ranges.len(), 15);
    assert!(ranges.iter().all(|o| (1..=4).contains(&o.depth) && o.args[0] >= 1 && o.args[1] <= 1 && o.tag == Some(0xd)));
    assert!(inv.ops.iter().filter(|o| o.kind != 6).all(|o| o.depth == 0));
    // a RANGE under 255/11 is refused (kind 6 is unknown there)
    let mut f = busy_frame(&[(6000, 36_000)], Blk::default(), None, 255);
    let at = f.chunks.iter().position(|c| c.ty == 35).unwrap();
    f.chunks[at].payload[16 + 144 + 12] = 6;
    assert!(reader::read(&f.bytes()).is_err());
}

// ---- blocked flip waits as RSX-bound evidence (CONFIG masks 1023/11) ----------------------------------------------

/// busy_frame (no FIFO wait, no polling) with one flip-wait scope at PPU `scope` recorded in the row (count and ticks
/// at 12/56, the scope block at 448 under 1023/11), and optionally with anchors that cannot qualify a map.
fn flip_frame(clears: &[(u64, u64)], scope: (u64, u64), masks: u32, anchored: bool) -> File {
    let mut f = busy_frame(clears, Blk::default(), None, masks);
    let at = f.chunks.iter().position(|c| c.ty == 36).unwrap();
    let row = &mut f.chunks[at].payload;
    row[12..16].copy_from_slice(&1u32.to_le_bytes());
    row[56..64].copy_from_slice(&(scope.1 - scope.0).to_le_bytes());
    if masks & 512 != 0 {
        row[448..456].copy_from_slice(&scope.0.to_le_bytes());
        row[456..464].copy_from_slice(&scope.1.to_le_bytes());
    }
    if !anchored {
        // the same nine anchors all at one coordinate: fewer than four distinct, so the map does not qualify
        let mut k = 0;
        for c in f.chunks.iter_mut().filter(|c| c.ty == 38) {
            k += 1;
            let mut a = anchor(k, 1000, (1999, 2001));
            a.labels = (0, 2 * a.id);
            *c = clock(&a);
        }
    }
    f
}

/// RSX busy through a blocked flip wait is RSX-bound evidence; a wait that mostly continues after the RSX finished
/// is not (pacing when the RSX was short, as today); without a qualified clock map the evidence is unusable.
#[test]
fn blocked_flip_wait_needs_certain_rsx_overlap() {
    // the anchors map raw x to PPU ~2x: a clear at raw 6000..36000 is busy at PPU ~12000..72000 (60% of 100000)
    // phase A: blocked 20000..70000 while the RSX is busy throughout
    let f = busy_read(flip_frame(&[(6000, 36_000)], (20_000, 70_000), 1023, true));
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    assert!(f.reason.starts_with("busy flip wait: RSX busy time >= "), "{}", f.reason);
    // the RSX finishes early (busy ~12000) and the wait runs on to the flip: PACING as today
    let f = busy_read(flip_frame(&[(6000, 12_000)], (26_000, 100_000), 1023, true));
    assert_eq!(f.verdict, Verdict::Pacing, "{}", f.reason);
    // RSX busy 60% of the frame, the wait mostly after it ended (12000 of 48000 overlap): not RSX-bound
    let f = busy_read(flip_frame(&[(6000, 36_000)], (60_000, 108_000), 1023, true));
    assert_ne!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    // overlap just below 3/4 of the scope: not RSX-bound; at 3/4 or more: RSX-bound
    let f = busy_read(flip_frame(&[(6000, 36_000)], (40_000, 84_000), 1023, true)); // 32000 of 44000: 73%
    assert_ne!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    let f = busy_read(flip_frame(&[(6000, 36_000)], (40_000, 80_000), 1023, true)); // 32000 of 40000: 80%
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    // a blocked wait with short busy time: UNKNOWN
    let f = busy_read(flip_frame(&[(6000, 6001), (36_000, 36_001)], (20_000, 70_000), 1023, true));
    assert_ne!(f.verdict, Verdict::RsxBound, "{}", f.reason);
    // no qualified clock map: the evidence is unusable, the frame UNKNOWN as today
    let f = busy_read(flip_frame(&[(6000, 36_000)], (20_000, 70_000), 1023, false));
    assert_eq!(f.verdict, Verdict::Unknown, "{}", f.reason);
    assert!(f.reason.contains("clock map unqualified"), "{}", f.reason);
    // older masks: no scope block, no flip-wait rule (today's verdict)
    let old = busy_read(flip_frame(&[(6000, 36_000)], (20_000, 70_000), 511, true));
    assert_eq!(old.verdict, Verdict::Unknown, "{}", old.reason);
    assert!(old.reason.starts_with("unresolved: FIFO wait >= 0"), "{}", old.reason);
}

/// The scope block must describe the row's scopes: inside the frame, in order, summing to the row's ticks.
#[test]
fn flip_wait_scope_block_is_checked() {
    assert!(reader::read(&flip_frame(&[(6000, 36_000)], (20_000, 70_000), 1023, true).bytes()).is_ok());
    let mut f = flip_frame(&[(6000, 36_000)], (20_000, 70_000), 1023, true);
    let at = f.chunks.iter().position(|c| c.ty == 36).unwrap();
    f.chunks[at].payload[56..64].copy_from_slice(&40_000u64.to_le_bytes()); // ticks disagree with the scope
    assert!(reader::read(&f.bytes()).is_err());
    let f = flip_frame(&[(6000, 36_000)], (5_000, 70_000), 1023, true); // begins before the frame
    assert!(reader::read(&f.bytes()).is_err());
}

/// The inner PPU bound of a raw timestamp comes from the nearest anchors and contains the anchor's own bracket.
#[test]
fn raw_to_ppu_bounds_from_anchors() {
    let r = reader::read(&flip_frame(&[(6000, 36_000)], (20_000, 70_000), 1023, true).bytes()).expect("reads");
    let inv = r.auto.as_ref().unwrap();
    let MapState::Qualified(m) = ps3dk_prof::auto_rsx::qualify(inv) else { panic!("qualified") };
    // anchor 1 is raw 1000 at PPU [1999, 2001]
    assert_eq!(ps3dk_prof::auto_rsx::raw_to_ppu_bounds(&inv.clocks, &m, 1000), Some((1999, 2001)));
    let (lo, hi) = ps3dk_prof::auto_rsx::raw_to_ppu_bounds(&inv.clocks, &m, 26_000).unwrap();
    assert!(lo <= 52_000 && 52_000 <= hi && hi - lo < 100, "{lo}..{hi}");
    assert_eq!(ps3dk_prof::auto_rsx::raw_to_ppu_bounds(&inv.clocks, &m, u64::MAX), None);
    // certain overlap: a busy interval mapped inward, intersected with the scope
    let o = ps3dk_prof::auto_rsx::certain_overlap(&inv.clocks, &m, &[(6000, 36_000)], &[(20_000, 70_000)]);
    assert!((49_900..=50_000).contains(&o), "{o}");
}

/// 1023/11 CONFIG names its two report-pair reserves; the range reserve is the default or an option leaving 64 pairs
/// for draws; other widths and masks are refused.
#[test]
fn config_names_the_reserves() {
    let f = busy_frame(&[(6000, 36_000)], Blk::default(), None, 1023);
    let r = reader::read(&f.bytes()).expect("reads");
    let a = r.auto.as_ref().unwrap().config.as_ref().unwrap().attached().unwrap().clone();
    assert_eq!(a.reserves, Some((32, 16)));
    let set = |f: &mut File, ranges: u32| {
        let at = f.chunks.iter().position(|c| c.ty == 34).unwrap();
        f.chunks[at].payload[164..168].copy_from_slice(&ranges.to_le_bytes());
    };
    let mut g = busy_frame(&[(6000, 36_000)], Blk::default(), None, 1023);
    set(&mut g, 32);
    let r = reader::read(&g.bytes()).expect("a range reserve of 32 reads");
    assert_eq!(r.auto.as_ref().unwrap().config.as_ref().unwrap().attached().unwrap().reserves, Some((32, 32)));
    let rep = analyze(&r);
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_text(&mut out, &rep).unwrap();
    assert!(String::from_utf8_lossy(&out).contains("report-pair reserves: 32 for whole-CALL brackets, 32 for AUTO ranges"));
    let mut out = Vec::new();
    ps3dk_prof::auto_rsx::write_json(&mut out, &rep).unwrap();
    let v: serde_json::Value = serde_json::from_slice(&out).unwrap();
    assert_eq!(v["config"]["range_pairs"], 32);
    set(&mut g, 504 - 32 - 64 + 1);
    assert!(reader::read(&g.bytes()).is_err(), "a range reserve leaving fewer than 64 pairs");
    // the 176-byte CONFIG only with 1023/11, and 1023/11 only with it
    let mut h = busy_frame(&[(6000, 36_000)], Blk::default(), None, 1023);
    let at = h.chunks.iter().position(|c| c.ty == 34).unwrap();
    h.chunks[at].payload.truncate(160);
    assert!(reader::read(&h.bytes()).is_err());
    let mut h = busy_frame(&[(6000, 36_000)], Blk::default(), None, 511);
    let at = h.chunks.iter().position(|c| c.ty == 34).unwrap();
    h.chunks[at].payload.extend_from_slice(&[0; 16]);
    assert!(reader::read(&h.bytes()).is_err());
    // older masks: no reserves
    let r = reader::read(&busy_frame(&[(6000, 36_000)], Blk::default(), None, 511).bytes()).expect("reads");
    assert_eq!(r.auto.as_ref().unwrap().config.as_ref().unwrap().attached().unwrap().reserves, None);
}

#[test]
fn call_covered_reported_in_text_and_json() {
    let rep = analyze(&reader::read(&call_frame(Some(((6500, 40_000), 3, 1, 1)), 1, 255).bytes()).unwrap());
    assert!(rep.call_bracket);
    let f = &rep.frames[0];
    assert_eq!(f.call_brackets, 1);
    assert_eq!(f.call_covered, 1);
    assert_eq!(f.segment_unrepresented, 1);

    // Text output verification
    let mut text_out = Vec::new();
    write_text(&mut text_out, &rep).unwrap();
    let text = String::from_utf8(text_out).unwrap();
    assert!(text.contains("unrepresented >= 1, call_covered 1, dropped builds 0"), "{}", text);

    // JSON output verification
    let mut json_out = Vec::new();
    write_json(&mut json_out, &rep).unwrap();
    let json_str = String::from_utf8(json_out).unwrap();
    let parsed: serde_json::Value = serde_json::from_str(&json_str).unwrap();
    assert_eq!(parsed["frames"][0]["call_covered"], 1);
    assert_eq!(parsed["frames"][0]["segment_unrepresented"], 1);

    // Golden check on older masks (call_bracket false): call_covered must not be emitted in JSON or text
    let rep_old = analyze(&reader::read(&call_frame(None, 1, 127).bytes()).unwrap());
    assert!(!rep_old.call_bracket);
    let f_old = &rep_old.frames[0];
    assert_eq!(f_old.call_covered, 0);

    let mut old_text_out = Vec::new();
    write_text(&mut old_text_out, &rep_old).unwrap();
    let old_text = String::from_utf8(old_text_out).unwrap();
    assert!(!old_text.contains("call_covered"), "{}", old_text);

    let mut old_json_out = Vec::new();
    write_json(&mut old_json_out, &rep_old).unwrap();
    let old_json_str = String::from_utf8(old_json_out).unwrap();
    let parsed_old: serde_json::Value = serde_json::from_str(&old_json_str).unwrap();
    assert!(parsed_old["frames"][0].get("call_covered").is_none());

    // Also verify golden fixture (console-r2) has call_bracket false and no call_covered
    let rep_fixture = read_fixture("console-r2");
    assert!(!rep_fixture.call_bracket);
    let mut fix_json_out = Vec::new();
    write_json(&mut fix_json_out, &rep_fixture).unwrap();
    let fix_json_str = String::from_utf8(fix_json_out).unwrap();
    let parsed_fix: serde_json::Value = serde_json::from_str(&fix_json_str).unwrap();
    for fr in parsed_fix["frames"].as_array().unwrap() {
        assert!(fr.get("call_covered").is_none());
    }
}
