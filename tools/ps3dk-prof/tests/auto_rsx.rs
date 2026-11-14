//! Automatic RSX observation (file_flags bit 7, chunk types 34-38): strict decode, refusals, the capture's own clock
//! map and the per-frame verdict.  Contract: docs/design/ps3dk-prof-auto-rsx.md.  The fixtures under
//! tests/fixtures/auto are captures written by the host runtime controls (tests/host/ps3dk-prof-runtime/
//! auto-runtime-test.py) with synthetic device facts; the synthetic files below are encoded byte by byte here.
mod common;
mod fifo_support;
use common::*;
use fifo_support::*;
use ps3dk_prof::auto_rsx::{analyze, Config, MapState, Report, Verdict};
use ps3dk_prof::{reader, Rule};

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
        if width == 144 {
            b.u64(o.tail.0).u32(o.tail.1).u16(o.tail.2).u16(o.tail.3);
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
