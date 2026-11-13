//! FIFO wrap phase events (file_flags bit 6, chunk types 31-33): strict decode, refusals, conservative pairing and
//! the CLI.  Contract: docs/design/ps3dk-prof-fifo.md.
mod common;
mod fifo_support;
use common::*;
use fifo_support::*;
use ps3dk_prof::fifo::{analyze, Phase, Report};
use ps3dk_prof::{reader, Rule};

fn rule(f: &File) -> Rule {
    reader::read(&f.bytes()).expect_err("must refuse").rule
}
fn report(f: &File) -> Report {
    analyze(&reader::read(&f.bytes()).expect("must read"))
}
fn complete(p: &Phase) -> (u64, u64) {
    match p {
        Phase::Complete { min, max, .. } => (*min, *max),
        other => panic!("not complete: {other:?}"),
    }
}

#[test]
fn flag_gate_full_and_partial_headers() {
    // The reader without bit 6 keeps types 31..33 unknown; bit 6 needs bit 0.
    let mut f = base(1);
    f.chunks.truncate(4);
    f.chunks.push(Cfg::default().chunk());
    assert_eq!(rule(&f), Rule::UnknownChunkType);
    for flags in [64u32, 66, 96, 80, 72] {
        let f = base(flags);
        assert_eq!(rule(&f), Rule::FileFlagsNonzero, "flags {flags}");
        assert_eq!(reader::read(&f.bytes()[..16]).unwrap_err().rule, Rule::FileFlagsNonzero, "flags {flags} header");
        for n in 13..16 {
            let e = reader::read(&f.bytes()[..n]).unwrap_err();
            assert_eq!(e.rule, Rule::FileFlagsNonzero, "flags {flags} cut {n}");
            assert!(e.detail.contains("bit 6 needs bit 0"), "{}", e.detail);
        }
    }
    for flags in [65u32, 67, 71, 73, 81, 97, 127] {
        let c = reader::read(&base(flags).bytes()[..13]).unwrap();
        assert!(c.fifo.is_some(), "partial flags {flags}");
    }
    // an unknown high flag bit refuses (byte 13 value 1 is bit 8, COUNTERS, which combines with bit 6)
    let mut b = base(65).bytes();
    b[13] = 2;
    assert_eq!(reader::read(&b).unwrap_err().rule, Rule::FileFlagsNonzero);
    // flag-clear partial diagnostics are unchanged
    let e = reader::read(&base(2).bytes()[..13]).unwrap_err();
    assert!(e.detail.ends_with("0, 1, 3 or 7"), "{}", e.detail);
}

#[test]
fn one_complete_wrap_pairs_exactly() {
    let w = wrap(1, 1, 1000, 500, 40);
    let f = with(vec![events(&w), St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk()]);
    let c = reader::read(&f.bytes()).unwrap();
    let inv = c.fifo.as_ref().unwrap();
    assert_eq!((inv.parsed_events, inv.visible_enter, inv.visible_exit), (6, 1, 1));
    let r = analyze(&c);
    assert_eq!(r.coverage, "FINAL");
    assert_eq!(r.wraps.len(), 1);
    let x = &r.wraps[0];
    assert_eq!(x.token, Some((OWNER, 7)));
    assert_eq!(complete(&x.phase1), (500, 502));
    assert_eq!(complete(&x.phase2), (40, 42));
    assert_eq!(complete(&x.outer), (547, 547));
}

#[test]
fn long_phase1_and_long_phase2_are_separate() {
    let mut evs = wrap(1, 1, 1000, 79_800_000, 10);
    evs.extend(wrap(7, 2, 80_000_000, 5, 39_900_000));
    let f = with(vec![events(&evs), St::new(1, 12, 12, 0, 12, 2, 2, 0).chunk()]);
    let r = report(&f);
    assert_eq!(complete(&r.wraps[0].phase1), (79_800_000, 79_800_002));
    assert_eq!(complete(&r.wraps[0].phase2), (10, 12));
    assert_eq!(complete(&r.wraps[1].phase1), (5, 7));
    assert_eq!(complete(&r.wraps[1].phase2), (39_900_000, 39_900_002));
    let mut out = Vec::new();
    ps3dk_prof::fifo::write_text(&mut out, &r).unwrap();
    let text = String::from_utf8(out).unwrap();
    assert!(text.contains("wrap 1 token 1/7 tail 0x1fa0 requested 64 words: phase1 COMPLETE 79800000..79800002 ticks; \
        phase2 COMPLETE 10..12 ticks; outer COMPLETE 79800017..79800017 ticks"), "{text}");
    assert!(text.contains("not GPU busy or idle time"), "{text}");
    assert!(text.contains("Not recorded by the FIFO wrap observer: register samples"), "{text}");
    assert!(!text.contains("Not enabled in this capture"), "{text}");
}

#[test]
fn open_phase_is_never_zero_or_complete() {
    let w = wrap(1, 1, 1000, 500, 40);
    // parked in phase 1: ENTER, PHASE1_BEGIN and nothing else, no status
    let f = with(vec![events(&w[..2])]);
    let r = report(&f);
    assert_eq!(r.coverage, "PROVISIONAL: no FINAL status");
    assert_eq!(r.wraps[0].phase1, Phase::Open { since_tb: 1002 });
    assert_eq!(r.wraps[0].phase2, Phase::Unavailable("not observed"));
    assert_eq!(r.wraps[0].outer, Phase::Open { since_tb: 1000 });
    assert!(!r.wraps[0].completed && r.wraps[0].entered);
    // a lower-bound status that has seen no later attempt keeps it open; entered and completed stay separate
    let f = with(vec![events(&w[..2]), St::new(2, 2, 2, 0, 2, 1, 0, 1).chunk()]);
    let r = report(&f);
    assert_eq!(r.wraps[0].phase1, Phase::Open { since_tb: 1002 });
    let s = r.status.unwrap();
    assert_eq!((s.wraps_started, s.wraps_completed), (1, 0));
    // the owner unacknowledged at seal: still OPEN, totals declared unavailable
    let f = with(vec![events(&w[..2]), St::new(1 | 2 | 8, 2, 2, 0, 2, 1, 0, 1).chunk()]);
    let r = report(&f);
    assert_eq!(r.coverage, "FINAL (lower bounds)");
    assert_eq!(r.wraps[0].phase1, Phase::Open { since_tb: 1002 });
    let mut out = Vec::new();
    ps3dk_prof::fifo::write_text(&mut out, &r).unwrap();
    let t = String::from_utf8(out).unwrap();
    assert!(t.contains("phase1 OPEN since tb 1002 (end UNKNOWN)") && t.contains("OWNER_UNACK"), "{t}");
    // later attempts the ring dropped: the end was lost, not open
    let f = with(vec![events(&w[..2]), St::new(2, 4, 2, 2, 2, 1, 0, 1).chunk()]);
    let r = report(&f);
    assert_eq!(r.wraps[0].phase1, Phase::Unavailable("end lost"));
    assert_eq!(r.wraps[0].outer, Phase::Unavailable("end lost"));
}

#[test]
fn lost_endpoint_never_pairs() {
    let w = wrap(1, 1, 1000, 500, 40);
    // PHASE1_END (seq 3) dropped: phase 1 lost, phase 2 exact, outer unavailable
    let vis: Vec<Ev> = w.iter().copied().filter(|e| e.seq != 3).collect();
    let f = with(vec![events(&vis), St::new(1, 6, 5, 1, 6, 1, 1, 0).chunk()]);
    let r = report(&f);
    assert_eq!(r.wraps[0].phase1, Phase::Unavailable("end lost"));
    assert_eq!(complete(&r.wraps[0].phase2), (40, 42));
    assert_eq!(r.wraps[0].outer, Phase::Unavailable("events between lost"));
    // PHASE1_BEGIN dropped
    let vis: Vec<Ev> = w.iter().copied().filter(|e| e.seq != 2).collect();
    let f = with(vec![events(&vis), St::new(1, 6, 5, 1, 6, 1, 1, 0).chunk()]);
    assert_eq!(report(&f).wraps[0].phase1, Phase::Unavailable("begin lost"));
    // A BEGIN and END separated by a dropped attempt are not paired (the pairing must not cross a gap).
    let mut gap = w.clone();
    for e in gap.iter_mut().skip(2) {
        e.seq += 1;
    }
    let f = with(vec![events(&gap), St::new(1, 7, 6, 1, 7, 1, 1, 0).chunk()]);
    let r = report(&f);
    assert_eq!(r.wraps[0].phase1, Phase::Unavailable("events between lost"));
    assert_eq!(r.wraps[0].outer, Phase::Unavailable("events between lost"));
    // a whole wrap dropped between two recorded ones: wrap ids skip, both neighbours exact
    let mut evs = wrap(1, 1, 1000, 5, 5);
    evs.extend(wrap(13, 3, 5000, 5, 5));
    let f = with(vec![events(&evs), St::new(1, 18, 12, 6, 18, 3, 3, 0).chunk()]);
    let r = report(&f);
    assert_eq!(r.wraps.iter().map(|w| w.wrap_id).collect::<Vec<_>>(), vec![1, 3]);
    assert!(r.wraps.iter().all(|w| matches!(w.outer, Phase::Complete { .. })));
}

#[test]
fn invalid_clock_gives_no_elapsed() {
    let mut w = wrap(1, 1, 1000, 500, 40);
    w[2].flags |= 2;
    w[2].t0 = 5; // regressed raw values kept, flagged
    let f = with(vec![events(&w), St::new(1, 6, 6, 0, 6, 1, 1, 0).chunk()]);
    let r = report(&f);
    assert_eq!(r.wraps[0].phase1, Phase::Unavailable("clock invalid"));
    assert_eq!(complete(&r.wraps[0].phase2), (40, 42));
    // the same regression without the flag is MALFORMED
    w[2].flags &= !2;
    assert_eq!(rule(&with(vec![events(&w)])), Rule::FifoEvents);
}

#[test]
fn frame_zero_and_absent_token() {
    let mut w = wrap(1, 1, 1000, 1, 1);
    for e in &mut w {
        e.frame = 0;
    }
    assert_eq!(report(&with(vec![events(&w)])).wraps[0].token, Some((OWNER, 0)));
    for e in &mut w {
        e.flags = 0;
        e.frame_owner = 0;
    }
    assert_eq!(report(&with(vec![events(&w)])).wraps[0].token, None);
}

#[test]
fn config_refusals() {
    let cases: Vec<(&str, Cfg)> = vec![
        ("schema", Cfg { schema: 2, ..Cfg::default() }),
        ("features 0", Cfg { features: 0, ..Cfg::default() }),
        ("future feature", Cfg { features: 3, ..Cfg::default() }),
        ("instance 0", Cfg { instance: 0, ..Cfg::default() }),
        ("generation 0", Cfg { generation: 0, ..Cfg::default() }),
        ("record bytes", Cfg { record: 95, ..Cfg::default() }),
        ("ring small", Cfg { ring: 15, allocated: 15 * 96 + 4096, ..Cfg::default() }),
        ("ring big", Cfg { ring: 65537, allocated: 65537 * 96 + 4096, max: u32::MAX, ..Cfg::default() }),
        ("allocated", Cfg { allocated: 1024 * 96, ..Cfg::default() }),
        ("over ceiling", Cfg { max: 1024 * 96, ..Cfg::default() }),
        ("timebase", Cfg { freq: FREQ + 1, ..Cfg::default() }),
        ("misaligned", Cfg { begin: BEGIN + 2, ..Cfg::default() }),
        ("inverted", Cfg { begin: END, end: BEGIN, ..Cfg::default() }),
        ("jump bit", Cfg { end: 0x2000_0000, ..Cfg::default() }),
        ("domain", Cfg { domain: D + 1, ..Cfg::default() }),
        ("owner", Cfg { owner: 2, ..Cfg::default() }),
        ("r0", Cfg { r0: 1, ..Cfg::default() }),
        ("r1", Cfg { r1: 1, ..Cfg::default() }),
    ];
    for (name, c) in cases {
        let mut f = base(65);
        f.chunks[4] = c.chunk();
        assert_eq!(rule(&f), Rule::FifoConfig, "{name}");
    }
    // lengths
    for n in [63usize, 65] {
        let mut f = base(65);
        f.chunks[4].payload.resize(n, 0);
        assert_eq!(rule(&f), Rule::FifoConfig, "length {n}");
    }
    // a second CONFIG, a CONFIG before its domain, a CONFIG naming an SPU-less non-RSX id
    let mut f = base(65);
    f.chunks.push(Cfg::default().chunk());
    assert_eq!(rule(&f), Rule::FifoConfig);
    let mut f = base(65);
    f.chunks.swap(3, 4);
    assert_eq!(rule(&f), Rule::FifoConfig);
}

#[test]
fn events_refusals() {
    let w = wrap(1, 1, 1000, 5, 5);
    // before CONFIG
    let mut f = base(65);
    f.chunks.insert(4, events(&w));
    assert_eq!(rule(&f), Rule::FifoEvents);
    // framing
    let mut c = events(&w);
    c.payload[8] = 7; // count
    assert_eq!(rule(&with(vec![c])), Rule::FifoEvents);
    let mut c = events(&w);
    c.payload.truncate(16 + 95);
    c.payload[8..12].copy_from_slice(&1u32.to_le_bytes());
    assert_eq!(rule(&with(vec![c])), Rule::FifoEvents);
    let empty = events(&[]);
    assert_eq!(rule(&with(vec![empty])), Rule::FifoEvents);
    for at in [0usize, 2, 4, 12] {
        let mut c = events(&w);
        c.payload[at] ^= 1;
        assert_eq!(rule(&with(vec![c])), Rule::FifoEvents, "prefix byte {at}");
    }
    // per-field refusals on the second event
    let fields: Vec<(&str, fn(&mut Ev))> = vec![
        ("seq 0", |e| e.seq = 0),
        ("seq ceiling", |e| e.seq = 1 << 63),
        ("wrap 0", |e| e.wrap = 0),
        ("kind 0", |e| e.kind = 0),
        ("kind 7", |e| e.kind = 7),
        ("flag", |e| e.flags |= 4),
        ("owner", |e| e.owner = 2),
        ("token owner", |e| e.frame_owner = 2),
        ("begin", |e| e.begin += 4),
        ("tail misaligned", |e| e.tail += 1),
        ("tail beyond end", |e| e.tail = END + 4),
        ("tail at begin", |e| e.tail = BEGIN),
        ("requested", |e| e.words = 1024),
        ("target", |e| e.target = BEGIN),
        ("current", |e| e.current = BEGIN),
        ("bracket", |e| { e.t0 = 2000; e.t1 = 1999; }),
        ("z44", |e| e.z0 = 1),
        ("z76", |e| e.z1 = 1),
        ("z80", |e| e.z2 = 1),
        ("z88", |e| e.z3 = 1),
    ];
    for (name, edit) in fields {
        let mut x = w.clone();
        edit(&mut x[1]);
        assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "{name}");
    }
    // token fields without TOKEN_PRESENT
    let mut x = w.clone();
    for e in &mut x {
        e.flags = 0;
    }
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents);
    // ordering: repeated sequence, reordered kinds, contiguous non-successor, frozen fields, clock regression
    let mut x = w.clone();
    x[1].seq = 1;
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "repeated seq");
    let mut x = w.clone();
    x.swap(1, 2);
    x[1].seq = 2;
    x[2].seq = 3;
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "kind order");
    let x = vec![w[0], ev(2, 1, 3, 1001, 1002)];
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "skipped kind with no gap");
    let x = vec![w[5], ev(7, 3, 1, 2000, 2000)];
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "next wrap id must be +1 when contiguous");
    let mut x = w.clone();
    x[3].words = 65;
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "frozen requested words");
    let mut x = w.clone();
    x[3].frame = 8;
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "frozen token");
    let mut x = w.clone();
    x[3].t0 = 1001;
    assert_eq!(rule(&with(vec![events(&x)])), Rule::FifoEvents, "regressed clock");
    // ordering holds across chunks
    let f = with(vec![events(&w[..3]), events(&w[2..])]);
    assert_eq!(rule(&f), Rule::FifoEvents, "repeated across chunks");
    let f = with(vec![events(&w[..3]), events(&w[3..])]);
    assert_eq!(report(&f).wraps.len(), 1, "split chunks are one stream");
}

#[test]
fn status_refusals_and_conservation() {
    let w = wrap(1, 1, 1000, 5, 5);
    let ok = St::new(1, 6, 6, 0, 6, 1, 1, 0);
    reader::read(&with(vec![events(&w), ok.chunk()]).bytes()).unwrap();
    let bad: Vec<(&str, St)> = vec![
        ("schema", St { schema: 2, ..ok }),
        ("flags", St { flags: 16 | 1, ..ok }),
        ("instance", St { instance: 2, ..ok }),
        ("provisional without lower bound", St { flags: 0, ..ok }),
        ("unack without lower bound", St { flags: 1 | 8, ..ok }),
        ("written", St { written: 5, ..ok }),
        ("last", St { last: 5, ..ok }),
        ("attempted below", St { flags: 3, attempted: 5, ..ok }),
        ("completed above", St { flags: 3, completed: 2, started: 1, ..ok }),
        ("enter above started", St { flags: 3, started: 0, completed: 0, ..ok }),
        ("high water", St { hw: 1025, ..ok }),
        ("exact attempted", St { attempted: 7, ..ok }),
        ("exact open", St { open: 1, ..ok }),
        ("exact open count", St { started: 3, completed: 1, open: 3, ..ok }),
    ];
    for (name, s) in bad {
        assert_eq!(rule(&with(vec![events(&w), s.chunk()])), Rule::FifoStatus, "{name}");
    }
    let mut c = ok.chunk();
    c.payload.push(0);
    assert_eq!(rule(&with(vec![events(&w), c])), Rule::FifoStatus, "length");
    // before CONFIG
    let mut f = base(65);
    f.chunks.insert(4, ok.chunk());
    assert_eq!(rule(&f), Rule::FifoStatus);
    // decreasing counters and cleared saturation
    let p = St::new(2, 6, 6, 0, 6, 1, 1, 0);
    let f = with(vec![events(&w), St { refused: 3, ..p }.chunk(), St { refused: 2, ..p }.chunk()]);
    assert_eq!(rule(&f), Rule::FifoStatus, "decreasing");
    let f = with(vec![events(&w), St { flags: 2 | 4, ..p }.chunk(), p.chunk()]);
    assert_eq!(rule(&f), Rule::FifoStatus, "saturation cleared");
    // nothing FIFO after the FINAL
    for after in [events(&wrap(7, 2, 5000, 1, 1)), ok.chunk(), Cfg::default().chunk()] {
        let ty = after.ty;
        let f = with(vec![events(&w), ok.chunk(), after]);
        let r = rule(&f);
        assert!(matches!(r, Rule::FifoEvents | Rule::FifoStatus | Rule::FifoConfig), "type {ty}: {r:?}");
    }
    // saturation relaxes the conservation equalities only
    let sat = St { flags: 1 | 4, attempted: 3, ..ok };
    reader::read(&with(vec![events(&w), sat.chunk()]).bytes()).unwrap();
    // an exact open wrap
    let f = with(vec![events(&w[..2]), St::new(1, 2, 2, 0, 2, 1, 0, 1).chunk()]);
    reader::read(&f.bytes()).unwrap();
    // drops conserve exactly
    let vis: Vec<Ev> = w.iter().copied().filter(|e| e.seq != 3).collect();
    reader::read(&with(vec![events(&vis), St::new(1, 6, 5, 1, 6, 1, 1, 0).chunk()]).bytes()).unwrap();
    assert_eq!(rule(&with(vec![events(&vis), St::new(1, 6, 5, 0, 6, 1, 1, 0).chunk()])), Rule::FifoStatus);
}

#[test]
fn every_byte_cut_is_torn_never_partial_evidence() {
    let mut evs = wrap(1, 1, 1000, 5, 5);
    evs.extend(wrap(7, 2, 3000, 5, 5));
    let f = with(vec![events(&evs[..6]), events(&evs[6..]), St::new(1, 12, 12, 0, 12, 2, 2, 0).chunk()]);
    let b = f.bytes();
    let whole = reader::read(&b).unwrap();
    assert_eq!(whole.fifo.as_ref().unwrap().parsed_events, 12);
    // chunk boundaries
    let mut ends = Vec::new();
    let mut pos = 16;
    while pos < b.len() {
        let len = u32::from_le_bytes(b[pos + 4..pos + 8].try_into().unwrap()) as usize;
        pos += 20 + len;
        ends.push(pos);
    }
    for n in 16..b.len() {
        let c = reader::read(&b[..n]).unwrap_or_else(|e| panic!("cut {n}: {e}"));
        let whole_chunks = ends.iter().filter(|&&e| e <= n).count();
        let inv = c.fifo.as_ref();
        let parsed = inv.map_or(0, |i| i.parsed_events);
        let expect = match whole_chunks {
            0..=5 => 0,
            6 => 6,
            _ => 12,
        };
        assert_eq!(parsed, expect, "cut {n}");
        assert_eq!(inv.is_some_and(|i| i.final_seen), whole_chunks == 8, "cut {n}");
        assert_eq!(inv.is_some_and(|i| i.config.is_some()), whole_chunks >= 5, "cut {n}");
    }
}

#[test]
fn budget_refusal_keeps_prior_evidence_and_validation() {
    let mut evs = wrap(1, 1, 1000, 5, 5);
    evs.extend(wrap(7, 2, 3000, 5, 5));
    let f = with(vec![events(&evs[..6]), events(&evs[6..]), St::new(1, 12, 12, 0, 12, 2, 2, 0).chunk()]);
    let c = reader::read_with_fifo_budgets(&f.bytes(), ps3dk_prof::fifo::Budgets { events: 8, admission_ops: 1 << 20 }).unwrap();
    let inv = c.fifo.as_ref().unwrap();
    assert_eq!(inv.refusal, Some("FIFO event retention budget"));
    assert_eq!((inv.events.len(), inv.parsed_events), (6, 12));
    let r = analyze(&c);
    assert!(r.wraps.is_empty() && r.refusal.is_some(), "no wrap analysis after a refusal (never an empty success)");
    // the admission charge (16 per retained event) refuses retention, never validation or the other feature data
    let c = reader::read_with_fifo_budgets(&f.bytes(), ps3dk_prof::fifo::Budgets { events: 1 << 20, admission_ops: 100 })
        .unwrap();
    let inv = c.fifo.as_ref().unwrap();
    assert_eq!(inv.refusal, Some("FIFO retention admission budget"));
    assert_eq!((inv.events.len(), inv.parsed_events, inv.admission_ops_used), (6, 12, 96));
    assert!(inv.final_seen, "the status after the refusal is still validated and read");
    // validation does not relax after the refusal
    let mut bad = evs.clone();
    bad[8].z0 = 1;
    let f = with(vec![events(&bad[..6]), events(&bad[6..])]);
    let e = reader::read_with_fifo_budgets(&f.bytes(), ps3dk_prof::fifo::Budgets { events: 3, admission_ops: 1 << 20 });
    assert_eq!(e.unwrap_err().rule, Rule::FifoEvents);
}

#[test]
fn cli_text_and_json() {
    let dir = std::env::temp_dir().join(format!("ps3dk-prof-fifo-cli-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let w = wrap(1, 1, 1000, 500, 40);
    let path = dir.join("open.prof");
    std::fs::write(&path, with(vec![events(&w), events(&wrap(7, 2, 5000, 1, 1)[..2])]).bytes()).unwrap();
    let exe = env!("CARGO_BIN_EXE_ps3dk-prof");
    let out = std::process::Command::new(exe).args(["fifo", path.to_str().unwrap()]).output().unwrap();
    let text = String::from_utf8(out.stdout).unwrap();
    assert!(text.starts_with("FIFO wrap waits (PPU clock): PROVISIONAL: no FINAL status\n"), "{text}");
    assert!(text.contains("status: none; loss and refusal counts UNKNOWN"), "{text}");
    assert!(text.contains("visible: 8 event(s), 2 WRAP_ENTER, 1 WRAP_EXIT"), "{text}");
    assert!(text.contains("wrap 2 token 1/7 tail 0x1fa0 requested 64 words: phase1 OPEN since tb 5002 (end UNKNOWN); \
        phase2 UNAVAILABLE (not observed); outer OPEN since tb 5000 (end UNKNOWN)"), "{text}");
    let out = std::process::Command::new(exe).args(["fifo", "--json", path.to_str().unwrap()]).output().unwrap();
    let v: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(v["gpu_execution"], "UNKNOWN");
    assert_eq!(v["wraps"][1]["phase1"]["state"], "OPEN");
    assert!(v["wraps"][1]["phase1"]["end"].is_null());
    assert_eq!(v["wraps"][0]["phase1"]["min_ticks"], 500);
    assert_eq!(v["visible"]["wrap_enter"], 2);
    // a capture without bit 6
    let plain = dir.join("plain.prof");
    let mut f = File::new(vec![chunk(T_HEADER, Hdr { max_threads: 64, ..Default::default() }.payload()),
        chunk(T_CLOCKS, clocks_payload(1, 1, 0, FREQ, 0))]);
    f.flags = 0;
    std::fs::write(&plain, f.bytes()).unwrap();
    let out = std::process::Command::new(exe).args(["fifo", plain.to_str().unwrap()]).output().unwrap();
    assert_eq!(String::from_utf8(out.stdout).unwrap().lines().next(), Some("FIFO wrap waits (PPU clock): UNSUPPORTED"));
    // a malformed one
    let bad = dir.join("bad.prof");
    std::fs::write(&bad, base(64).bytes()).unwrap();
    let out = std::process::Command::new(exe).args(["fifo", bad.to_str().unwrap()]).output().unwrap();
    assert_eq!(out.status.code(), Some(2));
    // usage
    let out = std::process::Command::new(exe).args(["fifo", "--bogus", plain.to_str().unwrap()]).output().unwrap();
    assert_eq!(out.status.code(), Some(3));
}
