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
    }
}
fn ops(list: &[AOp]) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u32(list.len() as u32).u32(128);
    for o in list {
        b.u64(o.id).u32(o.owner).u16(o.kind).u16(o.flags).u64(o.frame).u64(o.tb.0).u64(o.tb.1).u64(o.raw.0)
            .u64(o.raw.1).u64(0x1234);
        for a in o.args {
            b.u32(a);
        }
        b.u32(o.r0).u32(o.reports.0).u32(o.reports.1).u32(o.serials.0).u32(o.serials.1).u32(o.generation).u32(o.r1)
            .u64(o.published);
    }
    chunk(35, b.0)
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
}
/// A complete row with `clears` observed clears.
fn row(frame: u64, tb: (u64, u64), clears: u64) -> ARow {
    let mut rows = [[0u64; 12]; 3];
    rows[2][0] = clears;
    rows[2][1] = clears;
    ARow { flags: 3, owner: OWNER, frame, tb, unsupported: 0, lost: 0, rows }
}
fn coverage(r: &ARow) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(r.flags).u32(1).u32(r.owner).u32(0).u64(r.frame).u64(r.tb.0).u64(r.tb.1).u64(r.unsupported)
        .u64(r.lost).u64(0);
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
    assert_eq!(v, [Verdict::RsxBound, Verdict::PpuBound, Verdict::Mixed, Verdict::Unknown], "{:#?}", r.frames);
    assert!(r.frames[0].reason.starts_with("fetch:"), "{}", r.frames[0].reason);
    assert!(r.frames[1].reason.contains("not distinguished"));
    assert!(r.frames[3].reason.starts_with("unresolved"));
    let (lo, hi) = r.frames[0].rsx_span_ppu.unwrap();
    assert!(lo <= 60_000 && 60_000 <= hi && lo >= 50_000, "{lo}..{hi}");
    assert!(r.frames.iter().all(|f| f.verdict != Verdict::Pacing), "no presentation evidence: never PACING");
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
    assert_eq!(f.verdict, Verdict::RsxBound, "{}", f.reason);
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
    let c = reader::read(&seg_file(vec![ops(&[o]), status(1 | 2 | 64, n)]).bytes()).expect("reads");
    assert_eq!(c.auto.unwrap().last_status.unwrap().flags, 67);
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
