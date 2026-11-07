//! Domain extensions (file_flags bit 0, chunk types 8..16): the host reader increment of the P3.1 spec r5.
//! Every case pins the exact verdict: MALFORMED with its detail, or the per-source STRUCTURE / RAW VALIDITY /
//! COVERAGE. MAPPING is always UNMEASURED. Set EXT_FIXTURE_DIR to also write each fixture there (for the
//! baseline-reader comparison script).

mod common;

use common::*;
use ps3dk_prof::ext::{
    Bracket, EpochRecord, LossRange, OpenBegins, Pair, RawEvent, SourceKey, SourceReport, Structure,
};
use ps3dk_prof::{Outcome, Rule};

const T_DOMAIN: u32 = 8;
const T_EPOCH: u32 = 9;
const T_CAL: u32 = 10;
const T_IMAGES: u32 = 11;
const T_LANES: u32 = 12;
const T_SPU: u32 = 13;
const T_GPU: u32 = 14;
const T_LOSS: u32 = 15;
const T_FINAL: u32 = 16;
const LANE: u32 = 10;
const SPU_D: u32 = 2;
const RSX_D: u32 = 3;
const GPU_DESC: u32 = 3;

// ---- a test-only extension writer ---------------------------------------------------------------------------------

#[allow(clippy::too_many_arguments)]
fn domain_entry(b: &mut Buf, id: u32, kind: u8, width: u8, dir: u8, source_kind: u32, source_ref: u32, name: &str) {
    b.u32(id).u8(kind).u8(width).u8(dir).u8(0).u32(source_kind).u32(source_ref).u64(0).u64(0).u64(0).u64(0).str(name);
}
fn domains(entries: &[(u32, u8, u32)]) -> Chunk {
    // (id, kind, source_ref): SPU_DEC = 32-bit down lane, RSX = 64-bit up GCM
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
fn images(index: u32, build_id: &[u8], descs: u32) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(1).u32(index).bytes_str(build_id).u32(descs);
    for i in 0..descs {
        b.u32(i).u32(0x100 + i).u32(10 + i).str(&format!("spuzone{i}")).str("spu/main.c");
    }
    chunk(T_IMAGES, b.0)
}
fn lanes(entries: &[(u32, u32, u32, u32)]) -> Chunk {
    // (lane_id, domain_id, image, frame_track_owner)
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
fn loss(source: u32, kind: u8, first: u64, count: u64) -> Chunk {
    let mut b = Buf(Vec::new());
    b.u32(source).u8(kind).u8(1).u16(0).u64(first).u64(count).u32(0);
    chunk(T_LOSS, b.0)
}
fn finals(entries: &[(u32, u8, u8, u64, u64, u64, u32)]) -> Chunk {
    // (source, source_kind, state, next_seq, events, lost, open_begins)
    let mut b = Buf(Vec::new());
    b.u32(entries.len() as u32);
    for &(s, k, st, next, ev, lost, open) in entries {
        b.u32(s).u8(k).u8(st).u16(0).u64(next).u64(ev).u64(lost).u32(open);
    }
    chunk(T_FINAL, b.0)
}

/// The v1 part: one PPU incarnation (1) with two frames (ids 0 and 1) and a scope-2 descriptor (index 3).
fn v1_scenario() -> Scenario {
    let mut s = scenario(vec![lane(
        1,
        seqd(
            0,
            [vec![frame(0, 100, 0)], zone(0, 0, 0, 110, 1, 0), vec![end(0, 0, 0, 120, 1), frame(0, 200, 1)]].concat(),
        ),
    )]);
    let mut gd = desc(GPU_DESC, "gpuzone", "");
    gd.scope = 2;
    s.descs.push(gd);
    s
}

/// A complete, valid extension file: v1 scenario + `ext` chunks inserted before the TRAILER, flags = 1.
fn ext_file(ext: Vec<Chunk>) -> File {
    let mut f = v1_scenario().file();
    let at = f.find(T_TRAILER);
    for (i, c) in ext.into_iter().enumerate() {
        f.chunks.insert(at + i, c);
    }
    f.flags = 1;
    f
}

/// The standard valid extension chunks: one SPU lane (one zone) and one RSX domain (one zone in PPU frame 0).
fn std_ext() -> Vec<Chunk> {
    vec![
        domains(&[(SPU_D, 1, LANE), (RSX_D, 2, 0)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 1, 1, 0)]),
        calibration(SPU_D, 1, 0, &[(1000, 10, 20), (900, 30, 40)]),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0)]),
        gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 1, 0), (6_000, GPU_DESC, 2, 0, 1, 0)]),
        finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]),
    ]
}

fn dump(name: &str, bytes: &[u8]) {
    if let Ok(dir) = std::env::var("EXT_FIXTURE_DIR") {
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(format!("{dir}/{name}.prof"), bytes).unwrap();
    }
}

fn sources(name: &str, bytes: &[u8]) -> Vec<SourceReport> {
    dump(name, bytes);
    match load(bytes) {
        Outcome::Analysed(a) => a.capture.ext.expect("extension capture").sources,
        Outcome::Malformed(m) => panic!("{name}: expected a readable file, got {m}"),
    }
}

fn source(v: &[SourceReport], key: SourceKey) -> &SourceReport {
    v.iter().find(|s| s.key == key).unwrap_or_else(|| panic!("no source {key}"))
}

fn expect_malformed(name: &str, bytes: &[u8], detail: &str) {
    dump(name, bytes);
    match load(bytes) {
        Outcome::Malformed(m) => {
            assert_eq!(m.rule, Rule::Extension, "{name}: {m}");
            assert!(m.detail.contains(detail), "{name}: detail {:?} lacks {detail:?}", m.detail);
        }
        Outcome::Analysed(a) => panic!("{name}: expected MALFORMED ({detail}), got {:?}", a.verdict),
    }
}

fn with(mut ext: Vec<Chunk>, at: usize, c: Chunk) -> Vec<Chunk> {
    ext[at] = c;
    ext
}

// ---- T0: flags and versions -------------------------------------------------------------------------------------

#[test]
fn t0_valid_extension_file_reads_with_layers() {
    let v = sources("t0-valid", &ext_file(std_ext()).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.structure, l.raw_valid, l.paired(), l.events_total), (Structure::Complete, true, 1, 2));
    assert!(l.coverage_unknown.is_empty(), "{:?}", l.coverage_unknown);
    assert_eq!(l.open_begins, OpenBegins::Verified(0));
    let g = source(&v, SourceKey::Gpu(RSX_D));
    assert_eq!((g.structure, g.raw_valid, g.frames_resolved, g.frames_unresolved), (Structure::Complete, true, 1, 0));
}

#[test]
fn t0_flags_zero_rejects_an_extension_chunk() {
    let mut f = ext_file(std_ext());
    f.flags = 0;
    dump("t0-flags0-ext", &f.bytes());
    match load(&f.bytes()) {
        Outcome::Malformed(m) => assert_eq!(m.rule, Rule::UnknownChunkType),
        o => panic!("{o:?}"),
    }
}

#[test]
fn t0_flags_one_with_trailer_and_no_domain_is_a_valid_empty_extension() {
    // Spec r6: the flag declares the permitted format, not a producer.  0 sources, explicitly reported.
    let mut f = v1_scenario().file();
    f.flags = 1;
    let bytes = f.bytes();
    let v = sources("t0-flags1-nodomain", &bytes);
    assert!(v.is_empty());
    for cmd in ["check", "report"] {
        let (code, out) = cli_cmd(cmd, &bytes, &format!("empty-{cmd}"));
        assert_eq!(code, Some(0), "{cmd}: {out}");
        assert_eq!(out.matches("domain extensions: 0 sources declared").count(), 1, "{cmd}: {out}");
        assert!(!out.contains("MAPPING"), "{cmd}: {out}");
    }
    // Nonempty but unbound at the TRAILER stays MALFORMED (the same file plus one forward-referencing domain).
    expect_malformed("t0-flags1-unbound", &ext_file(vec![domains(&[(SPU_D, 1, LANE)])]).bytes(), "never declared");
}

#[test]
fn t0_flags_one_torn_before_any_domain_is_incomplete() {
    let mut f = v1_scenario().file().without_trailer();
    f.flags = 1;
    let bytes = f.bytes();
    dump("t0-flags1-torn-nodomain", &bytes);
    match load(&bytes) {
        Outcome::Analysed(a) => {
            assert!(matches!(a.verdict, ps3dk_prof::Verdict::Incomplete(_)));
            assert!(a.capture.ext.as_ref().unwrap().sources.is_empty());
        }
        o => panic!("{o:?}"),
    }
}

#[test]
fn t0_unknown_flag_bits_are_malformed_in_every_byte() {
    // Bit 1 (PROFILE_RECORD, profile-format-spec.md 2) is now defined: alone (2) it is FileFlagsNonzero (it requires
    // bit 0); with bit 0 (3) the file must carry a PROFILE at chunk 2, which this file does not (ProfileMissing).
    for flags in [2u32, 4, 7, 0x100, 0x10000, 0x8000_0000] {
        let mut f = ext_file(std_ext());
        f.flags = flags;
        dump(&format!("t0-flags-{flags:x}"), &f.bytes());
        match load(&f.bytes()) {
            Outcome::Malformed(m) => assert_eq!(m.rule, Rule::FileFlagsNonzero, "flags {flags:#x}"),
            o => panic!("flags {flags:#x}: {o:?}"),
        }
    }
    let mut f = ext_file(std_ext());
    f.flags = 3;
    match load(&f.bytes()) {
        Outcome::Malformed(m) => assert_eq!(m.rule, Rule::ProfileMissing, "flags 3 without a PROFILE"),
        o => panic!("flags 3: {o:?}"),
    }
}

#[test]
fn t0_every_header_cut_of_a_flags_one_file_is_incomplete_in_the_new_reader() {
    let bytes = ext_file(std_ext()).bytes();
    for n in 0..=16 {
        dump(&format!("t0-cut-{n:02}"), &bytes[..n]);
        match load(&bytes[..n]) {
            Outcome::Analysed(a) => assert!(matches!(a.verdict, ps3dk_prof::Verdict::Incomplete(_)), "cut {n}"),
            Outcome::Malformed(m) => panic!("cut {n}: {m}"),
        }
    }
}

#[test]
fn t0_flags_zero_files_with_any_minor_are_unchanged_v1() {
    for minor in [0u16, 1, 7, 65535] {
        let mut f = v1_scenario().file();
        f.minor = minor;
        dump(&format!("t0-v1-minor{minor}"), &f.bytes());
        match load(&f.bytes()) {
            Outcome::Analysed(a) => {
                assert_eq!(a.verdict, ps3dk_prof::Verdict::Complete);
                assert!(a.capture.ext.is_none());
                assert_eq!(a.capture.minor_warning, minor > 0);
            }
            o => panic!("minor {minor}: {o:?}"),
        }
    }
}

// ---- T1: layouts and bounds --------------------------------------------------------------------------------------

#[test]
fn t1_spu_events_at_and_over_the_cap() {
    let n = 131_069usize;
    let mut ev = Vec::with_capacity(n);
    for i in 0..n {
        ev.push(((n - i) as u32, 0u16, 3u8, 0u8)); // MARKs at depth 0
    }
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &ev);
    ext[7] = finals(&[(LANE, 1, 1, n as u64, n as u64, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t1-spu-max", &ext_file(ext.clone()).bytes());
    assert_eq!(source(&v, SourceKey::Lane(LANE)).marks(), n as u64);
    // n + 1 does not fit the chunk cap: the reader refuses its len before any extension rule.
    ev.push((1, 0, 3, 0));
    ext[5] = spu_events(LANE, 1, 0, &ev);
    match load(&ext_file(ext).bytes()) {
        Outcome::Malformed(m) => assert_eq!(m.rule, Rule::ChunkLenOverCap),
        o => panic!("{o:?}"),
    }
}

#[test]
fn t1_count_and_length_mismatch() {
    let mut c = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0)]);
    c.payload.truncate(c.payload.len() - 1);
    expect_malformed("t1-spu-len", &ext_file(with(std_ext(), 5, c)).bytes(), "SPU_EVENTS n 2 with payload length");
    let mut c = gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 1, 0)]);
    c.payload[16..20].copy_from_slice(&0u32.to_le_bytes());
    expect_malformed("t1-gpu-n0", &ext_file(with(std_ext(), 6, c)).bytes(), "GPU_EVENTS n 0");
}

#[test]
fn t1_domain_combinations_and_reserved() {
    for (kind, width, dir, sk, sr, want) in [
        (1u8, 64u8, 1u8, 1u32, LANE, "kind 1 width 64"),
        (1, 32, 2, 1, LANE, "direction 2"),
        (2, 32, 2, 2, 0, "kind 2 width 32"),
        (2, 64, 2, 2, 5, "source_ref 5"),
        (3, 32, 1, 1, LANE, "kind 3"),
    ] {
        let mut b = Buf(Vec::new());
        b.u32(1);
        domain_entry(&mut b, SPU_D, kind, width, dir, sk, sr, "x");
        expect_malformed(
            &format!("t1-combo-{kind}-{width}-{dir}"),
            &ext_file(vec![chunk(T_DOMAIN, b.0)]).bytes(),
            want,
        );
    }
    let mut d = domains(&[(SPU_D, 1, LANE)]);
    d.payload[20] = 1; // first reserved u64
    expect_malformed("t1-domain-reserved", &ext_file(vec![d]).bytes(), "reserved field nonzero");
    expect_malformed("t1-domain-id1", &ext_file(vec![domains(&[(1, 2, 0)])]).bytes(), "out of 2..65535");
    expect_malformed(
        "t1-domain-dup",
        &ext_file(vec![domains(&[(RSX_D, 2, 0), (RSX_D, 2, 0)])]).bytes(),
        "declared twice",
    );
}

#[test]
fn t1_images_dense_and_raw_build_id() {
    // A non-UTF-8 build id is fine (raw bytes).
    let mut ext = std_ext();
    ext[1] = images(0, &[0xff, 0xfe, 0x00], 2);
    sources("t1-raw-build-id", &ext_file(ext).bytes());
    // A duplicated descriptor index (so one is missing): not dense.
    let mut b = Buf(Vec::new());
    b.u32(1).u32(0).bytes_str(b"b").u32(2);
    b.u32(0).u32(0).u32(1).str("a").str("f");
    b.u32(0).u32(0).u32(1).str("b").str("f");
    expect_malformed(
        "t1-image-dup-desc",
        &ext_file(with(std_ext(), 1, chunk(T_IMAGES, b.0))).bytes(),
        "declared twice",
    );
    expect_malformed("t1-image-desc0", &ext_file(with(std_ext(), 1, images(0, b"b", 0))).bytes(), "desc_count 0");
}

#[test]
fn t1_forbidden_epoch_reasons_and_first_reason() {
    for reason in [3u8, 4, 7] {
        let ext = with(std_ext(), 3, epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 1, 1, 0), (RSX_D, 2, reason, 0)]));
        expect_malformed(&format!("t1-reason-{reason}"), &ext_file(ext).bytes(), "not permitted");
    }
    let ext = with(std_ext(), 3, epochs(&[(SPU_D, 1, 2, 7), (RSX_D, 1, 1, 0)]));
    expect_malformed("t1-first-reason", &ext_file(ext).bytes(), "first epoch reason 2");
}

#[test]
fn t1_width_rules() {
    let ext = with(std_ext(), 4, calibration(SPU_D, 1, 0, &[(1 << 32, 10, 20)]));
    expect_malformed("t1-cal-width", &ext_file(ext).bytes(), "wider than 32 bits");
    let ext = with(std_ext(), 3, epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 1, 1, 0), (SPU_D, 2, 2, 1 << 32)]));
    expect_malformed("t1-written-width", &ext_file(ext).bytes(), "written_value 4294967296");
    let ext = with(std_ext(), 3, epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 1, 1, 0), (SPU_D, 2, 5, 9)]));
    expect_malformed("t1-written-nonwrite", &ext_file(ext).bytes(), "written_value 9 with reason 5");
}

// ---- T2: sequences and finalization ----------------------------------------------------------------------------

#[test]
fn t2_loss_tiles_and_makes_the_source_not_valid() {
    let mut ext = std_ext();
    ext.insert(6, loss(LANE, 1, 2, 3));
    ext[8] = finals(&[(LANE, 1, 1, 5, 2, 3, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t2-trailing-loss", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.structure, l.raw_valid, l.lost_total), (Structure::Complete, false, 3));
    assert!(l.coverage_unknown.iter().any(|c| c == "LOSS"));
    assert_eq!(l.open_begins, OpenBegins::Unverifiable(0));
}

#[test]
fn t2_gap_and_overlap_are_malformed() {
    let ext = with(std_ext(), 5, spu_events(LANE, 1, 1, &[(990, 0, 1, 0), (980, 0, 2, 0)]));
    expect_malformed("t2-gap", &ext_file(ext).bytes(), "events start at 1, want 0");
    let mut ext = std_ext();
    ext.insert(6, spu_events(LANE, 1, 1, &[(970, 0, 3, 0)]));
    expect_malformed("t2-overlap", &ext_file(ext).bytes(), "events start at 1, want 2");
    let mut ext = std_ext();
    ext.insert(6, loss(LANE, 1, 2, u64::MAX));
    expect_malformed("t2-loss-overflow", &ext_file(ext).bytes(), "sequence overflows");
}

#[test]
fn t2_never_assigned_and_all_lost_sources() {
    let mut ext = std_ext();
    ext.remove(5); // no SPU events
    ext[6] = finals(&[(LANE, 1, 1, 0, 0, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t2-never-assigned", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.raw_valid, l.events_total, l.lost_total), (true, 0, 0));
    let mut ext = std_ext();
    ext[5] = loss(LANE, 1, 0, 4);
    ext[7] = finals(&[(LANE, 1, 1, 4, 0, 4, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t2-all-lost", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.raw_valid, l.events_total, l.lost_total), (false, 0, 4));
}

#[test]
fn t2_final_count_mismatches_duplicates_and_data_after() {
    let ext = with(std_ext(), 7, finals(&[(LANE, 1, 1, 3, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]));
    expect_malformed("t2-next-mismatch", &ext_file(ext).bytes(), "FINAL next_seq 3");
    let ext = with(std_ext(), 7, finals(&[(LANE, 1, 1, 2, u64::MAX, 2, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]));
    expect_malformed("t2-sum-overflow", &ext_file(ext).bytes(), "overflows");
    let mut ext = std_ext();
    ext.push(finals(&[(RSX_D, 2, 1, 2, 2, 0, 0)]));
    expect_malformed("t2-dup-final", &ext_file(ext).bytes(), "a second DOMAIN_FINAL");
    let mut ext = std_ext();
    ext.push(gpu_events(RSX_D, 1, 2, &[(7_000, GPU_DESC, 3, 0, 1, 1)]));
    expect_malformed("t2-after-final", &ext_file(ext).bytes(), "data after its DOMAIN_FINAL");
    let ext = with(std_ext(), 7, finals(&[(LANE, 2, 1, 2, 2, 0, 0)]));
    expect_malformed("t2-final-kind", &ext_file(ext).bytes(), "undeclared GPU domain 10");
}

#[test]
fn t2_missing_final_and_torn_trailer_are_incomplete() {
    let mut ext = std_ext();
    ext.pop();
    let v = sources("t2-missing-final", &ext_file(ext).bytes());
    assert_eq!(source(&v, SourceKey::Lane(LANE)).structure, Structure::Incomplete);
    let v = sources("t2-final-no-trailer", &ext_file(std_ext()).without_trailer().bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!(l.structure, Structure::Incomplete);
    assert!(l.incomplete.iter().any(|w| w.contains("no valid TRAILER")));
}

// ---- T3: references and epochs ------------------------------------------------------------------------------------

#[test]
fn t3_forward_reference_rules() {
    let ext = vec![domains(&[(SPU_D, 1, LANE), (RSX_D, 2, 0)])];
    expect_malformed("t3-unbound-at-trailer", &ext_file(ext).bytes(), "which is never declared");
    let ext = vec![domains(&[(SPU_D, 1, LANE), (4, 1, LANE)])];
    expect_malformed("t3-ref-twice", &ext_file(ext).bytes(), "already referenced");
    let ext = vec![domains(&[(SPU_D, 1, LANE)]), images(0, b"b", 1), lanes(&[(11, SPU_D, 0, 0)])];
    expect_malformed("t3-ref-mismatch", &ext_file(ext).bytes(), "names lane 10");
    let ext =
        vec![domains(&[(SPU_D, 1, LANE)]), images(0, b"b", 1), lanes(&[(LANE, SPU_D, 0, 0), (LANE, SPU_D, 0, 0)])];
    expect_malformed("t3-lane-reused", &ext_file(ext).bytes(), "zero or reused");
    let ext = vec![domains(&[(RSX_D, 2, 0)]), images(0, b"b", 1), lanes(&[(LANE, RSX_D, 0, 0)])];
    expect_malformed("t3-lane-not-spu", &ext_file(ext).bytes(), "is not SPU_DEC");
    let ext = vec![domains(&[(SPU_D, 1, LANE)]), epochs(&[(SPU_D, 1, 1, 0)])];
    expect_malformed("t3-epoch-before-bind", &ext_file(ext).bytes(), "used before its lane 10 is declared");
    // A torn file whose lane was never declared: that SPU source is INCOMPLETE (unbound).
    let v = sources("t3-unbound-torn", &ext_file(vec![domains(&[(SPU_D, 1, LANE)])]).without_trailer().bytes());
    assert!(source(&v, SourceKey::Lane(LANE)).incomplete.iter().any(|w| w.starts_with("unbound")));
}

#[test]
fn t3_descriptor_owner_and_epoch_order() {
    let ext = with(std_ext(), 5, spu_events(LANE, 1, 0, &[(990, 2, 1, 0), (980, 2, 2, 0)]));
    expect_malformed("t3-spu-desc", &ext_file(ext).bytes(), "descriptor 2 >= 2");
    let ext = with(std_ext(), 6, gpu_events(RSX_D, 1, 0, &[(5_000, 0, 1, 0, 1, 0), (6_000, 0, 2, 0, 1, 0)]));
    expect_malformed("t3-gpu-desc-scope", &ext_file(ext).bytes(), "not a declared scope-2 descriptor");
    let ext =
        with(std_ext(), 6, gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 9, 0), (6_000, GPU_DESC, 2, 0, 9, 0)]));
    expect_malformed("t3-gpu-owner", &ext_file(ext).bytes(), "frame_owner 9 is not a declared incarnation");
    let ext = with(std_ext(), 3, epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 2, 1, 0)]));
    expect_malformed("t3-epoch-skip", &ext_file(ext).bytes(), "epoch 2 after 0");
    let mut ext = std_ext();
    ext.insert(6, epochs(&[(SPU_D, 2, 2, 0xffff_ffff)]));
    ext.insert(7, spu_events(LANE, 1, 2, &[(970, 0, 3, 0)]));
    expect_malformed("t3-older-epoch", &ext_file(ext).bytes(), "epoch 1 after epoch 2");
}

// ---- T4: pairing --------------------------------------------------------------------------------------------------

#[test]
fn t4_protocol_error_is_terminal_but_parsing_continues() {
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 2, 0)]); // END desc 1 vs open desc 0
    ext.insert(6, spu_events(LANE, 1, 2, &[(970, 0, 3, 0)]));
    ext[8] = finals(&[(LANE, 1, 1, 3, 3, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t4-protocol", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert!(!l.raw_valid);
    assert!(l.protocol_error.as_deref().unwrap().contains("does not match"));
    assert_eq!((l.paired(), l.marks()), (0, 0), "no pairing claims after the protocol error");
    assert_eq!(l.open_begins, OpenBegins::Unverifiable(0));
    assert_eq!(l.open_at_end(), None, "the open count after a protocol error is unknown, not 0");
    assert_eq!(l.raw.open, None);
    assert_eq!(l.raw.events.len(), 3, "every event is still decoded");
}

#[test]
fn t4_loss_censors_then_resyncs_at_depth_zero() {
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1)]);
    ext.insert(6, loss(LANE, 1, 2, 1));
    ext.insert(
        7,
        spu_events(LANE, 1, 3, &[(960, 1, 2, 1), (955, 0, 3, 1), (950, 0, 3, 0), (945, 1, 1, 0), (940, 1, 2, 0)]),
    );
    ext[9] = finals(&[(LANE, 1, 1, 8, 7, 1, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t4-loss-resync", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    // two open BEGINs censored; END + depth-1 MARK unpaired; depth-0 MARK recorded; BEGIN depth 0 resyncs; END pairs
    assert_eq!((l.censored(), l.unpaired(), l.marks(), l.paired()), (2, 2, 1, 1));
    assert_eq!(l.open_begins, OpenBegins::Unverifiable(0));
    assert!(l.protocol_error.is_none());
}

#[test]
fn t4_epoch_transition_goes_unknown() {
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0)]);
    ext.insert(6, epochs(&[(SPU_D, 2, 2, 0xffff_ffff)]));
    ext.insert(7, spu_events(LANE, 2, 1, &[(0xffff_0000, 0, 2, 0)]));
    ext[9] = finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t4-epoch", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.censored(), l.unpaired(), l.paired()), (1, 1, 0));
    assert!(!l.raw_valid, "open_begins cannot be verified after a transition");
}

#[test]
fn t4_open_begins_mismatch_with_known_history_is_malformed() {
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0), (970, 1, 1, 0)]);
    ext[7] = finals(&[(LANE, 1, 1, 3, 3, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    expect_malformed("t4-open-mismatch", &ext_file(ext.clone()).bytes(), "open_begins 0 but 1 BEGINs are open");
    ext[7] = finals(&[(LANE, 1, 1, 3, 3, 0, 1), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t4-open-verified", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.open_begins, l.open_at_end(), l.raw_valid), (OpenBegins::Verified(1), Some(1), true));
    assert!(l.coverage_unknown.iter().any(|c| c.contains("OPEN at FINAL")));
}

// ---- T4b: the depth-zero epoch rule (clock-mapping plan B r2 5.4) --------------------------------------------------
// Reader side only: these fixtures are written here, not by the runtime; the runtime is tested separately in the SPU
// host model (tests/host/ps3dk-prof-spu).

const B: u8 = 1;
const E: u8 = 2;
const UNKNOWN_OPEN: u32 = u32::MAX;

/// One SPU lane: epoch 1 (start), `body`, and a FINAL (FINALIZED) with `next`, `events`, `lost` and `open`.
fn lane_final(body: Vec<Chunk>, next: u64, events: u64, lost: u64, open: u32) -> Vec<Chunk> {
    let mut v = vec![
        domains(&[(SPU_D, 1, LANE)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0)]),
    ];
    v.extend(body);
    v.push(finals(&[(LANE, 1, 1, next, events, lost, open)]));
    v
}

fn lane_source(name: &str, ext: Vec<Chunk>) -> SourceReport {
    source(&sources(name, &ext_file(ext).bytes()), SourceKey::Lane(LANE)).clone()
}

/// A zone in epoch 1, a reason-2 transition at depth 0, a zone in epoch 2.
fn depth0_body() -> Vec<Chunk> {
    vec![
        spu_events(LANE, 1, 0, &[(990, 0, B, 0), (980, 0, E, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 2, &[(0xFFFF_0000, 1, B, 0), (0xFFFE_0000, 1, E, 0)]),
    ]
}

#[test]
fn b1_depth0_transition_keeps_the_history_known_and_verified() {
    let l = lane_source("b1-depth0", lane_final(depth0_body(), 4, 4, 0, 0));
    assert_eq!((l.censored(), l.unpaired(), l.paired()), (0, 0, 2));
    assert_eq!((l.open_begins, l.open_at_end(), l.raw_valid), (OpenBegins::Verified(0), Some(0), true));
    assert!(l.invalid.is_empty(), "{:?}", l.invalid);
    let (code, out) = cli(&ext_file(lane_final(depth0_body(), 4, 4, 0, 0)).bytes(), "b1-depth0");
    assert_eq!(code, Some(0), "{out}");
    assert!(out.contains("RAW VALIDITY VALID"), "{out}");
}

#[test]
fn b1_depth0_transition_with_a_zone_open_at_the_final_is_verified_with_its_count() {
    let mut body = depth0_body();
    body.push(spu_events(LANE, 2, 4, &[(0xFFFD_0000, 0, B, 0)]));
    let l = lane_source("b1-depth0-open", lane_final(body, 5, 5, 0, 1));
    assert_eq!((l.open_begins, l.open_at_end(), l.raw_valid), (OpenBegins::Verified(1), Some(1), true));
}

#[test]
fn b1_pending_end_in_the_chunk_before_the_epoch_closes_the_stack_first() {
    // the END that empties the stack is in its own chunk immediately before the EPOCH (the drain closes the walk's
    // events before it emits the EPOCH); the transition is then at depth 0
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, B, 0)]),
        spu_events(LANE, 1, 1, &[(980, 0, E, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 2, &[(0xFFFF_0000, 1, B, 0), (0xFFFE_0000, 1, E, 0)]),
    ];
    let l = lane_source("b1-pending-end", lane_final(body, 4, 4, 0, 0));
    assert_eq!((l.censored(), l.paired(), l.open_begins, l.raw_valid), (0, 2, OpenBegins::Verified(0), true));
}

#[test]
fn b1_depth1_transition_censors_and_stays_unverifiable() {
    // today's rule, unchanged: a BEGIN open at the transition is CENSORED and the history becomes UNKNOWN
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, B, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 1, &[(0xFFFF_0000, 0, E, 0)]),
    ];
    let l = lane_source("b1-depth1", lane_final(body, 2, 2, 0, 0));
    assert_eq!((l.censored(), l.unpaired(), l.paired()), (1, 1, 0));
    assert_eq!((l.open_begins, l.raw_valid), (OpenBegins::Unverifiable(0), false));
}

#[test]
fn b1_loss_then_a_depth0_transition_stays_unknown() {
    // LOSS, then a depth-0 resynchronisation (stack known and empty again), then a depth-0 transition: the history
    // was unknown once, so the transition does not keep it; the FINAL's value is reported, never verified
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, B, 0), (980, 0, E, 0)]),
        loss(LANE, 1, 2, 2),
        spu_events(LANE, 1, 4, &[(960, 0, B, 0), (950, 0, E, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 6, &[(0xFFFF_0000, 1, B, 0), (0xFFFE_0000, 1, E, 0)]),
    ];
    for (tag, open) in [("zero", 0u32), ("placeholder", UNKNOWN_OPEN), ("other", 3)] {
        let l = lane_source(&format!("b1-loss-depth0-{tag}"), lane_final(body.clone(), 8, 6, 2, open));
        assert_eq!((l.open_begins, l.raw_valid), (OpenBegins::Unverifiable(open), false), "{tag}");
        assert_eq!((l.censored(), l.paired()), (0, 3), "{tag}");
    }
}

#[test]
fn b1_an_already_unknown_history_after_a_protocol_error_stays_unknown() {
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, E, 0)]), // END on an empty stack: PROTOCOL ERROR
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 1, &[(0xFFFF_0000, 1, B, 0), (0xFFFE_0000, 1, E, 0)]),
    ];
    let l = lane_source("b1-protocol-depth0", lane_final(body, 3, 3, 0, 0));
    assert!(l.protocol_error.is_some());
    assert_eq!((l.open_begins, l.raw_valid, l.open_at_end()), (OpenBegins::Unverifiable(0), false, None));
}

#[test]
fn b1_a_second_transition_at_depth1_ends_the_known_history() {
    let mut body = depth0_body();
    body.push(spu_events(LANE, 2, 4, &[(0xFFFD_0000, 0, B, 0)]));
    body.push(epochs(&[(SPU_D, 3, 2, 0xFFFF_FFFF)]));
    body.push(spu_events(LANE, 3, 5, &[(0xFFFF_0000, 0, E, 0)]));
    let l = lane_source("b1-depth0-then-depth1", lane_final(body, 6, 6, 0, 0));
    assert_eq!((l.censored(), l.unpaired(), l.paired()), (1, 1, 2));
    assert_eq!((l.open_begins, l.raw_valid), (OpenBegins::Unverifiable(0), false));
}

#[test]
fn b1_final_mismatch_after_a_depth0_transition_is_malformed() {
    expect_malformed(
        "b1-final-mismatch-high",
        &ext_file(lane_final(depth0_body(), 4, 4, 0, 1)).bytes(),
        "open_begins 1 but 0 BEGINs are open",
    );
    let mut body = depth0_body();
    body.push(spu_events(LANE, 2, 4, &[(0xFFFD_0000, 0, B, 0)]));
    expect_malformed(
        "b1-final-mismatch-low",
        &ext_file(lane_final(body, 5, 5, 0, 0)).bytes(),
        "open_begins 0 but 1 BEGINs are open",
    );
}

#[test]
fn b1_historical_unknown_final_after_a_depth0_transition_stays_unverifiable() {
    // a runtime that predates the rule wrote UINT32_MAX after every transition: kept as written, never read as 0,
    // never MALFORMED, never verified
    let ext = lane_final(depth0_body(), 4, 4, 0, UNKNOWN_OPEN);
    let l = lane_source("b1-historical-placeholder", ext.clone());
    assert_eq!(l.open_begins, OpenBegins::Unverifiable(UNKNOWN_OPEN));
    assert!(!l.raw_valid && l.invalid.iter().any(|r| r == "open_begins not verified"), "{:?}", l.invalid);
    assert_eq!((l.censored(), l.paired(), l.open_at_end()), (0, 2, Some(0)));
    let (code, out) = cli(&ext_file(ext).bytes(), "b1-historical");
    assert_eq!(code, Some(4), "{out}");
    // with a zone still open at the FINAL, the placeholder is still not a count
    let mut body = depth0_body();
    body.push(spu_events(LANE, 2, 4, &[(0xFFFD_0000, 0, B, 0)]));
    let l = lane_source("b1-historical-placeholder-open", lane_final(body, 5, 5, 0, UNKNOWN_OPEN));
    assert_eq!((l.open_begins, l.raw_valid, l.open_at_end()), (OpenBegins::Unverifiable(UNKNOWN_OPEN), false, Some(1)));
}

#[test]
fn b1_the_placeholder_without_a_kept_transition_is_still_malformed() {
    // one epoch, history known: UINT32_MAX is no count and the rule does not apply (unchanged)
    let body = vec![spu_events(LANE, 1, 0, &[(990, 0, B, 0), (980, 0, E, 0)])];
    expect_malformed(
        "b1-placeholder-single-epoch",
        &ext_file(lane_final(body, 2, 2, 0, UNKNOWN_OPEN)).bytes(),
        "open_begins 4294967295 but 0 BEGINs are open",
    );
}

#[test]
fn b1_other_transition_reasons_still_go_unknown() {
    // the rule is the drain's: only a reason-2 (decrementer write) transition keeps the history; 5 and 6 do not
    for reason in [5u8, 6] {
        let body = vec![
            spu_events(LANE, 1, 0, &[(990, 0, B, 0), (980, 0, E, 0)]),
            epochs(&[(SPU_D, 2, reason, 0)]),
            spu_events(LANE, 2, 2, &[(0xFFFF_0000, 1, B, 0), (0xFFFE_0000, 1, E, 0)]),
        ];
        let l = lane_source(&format!("b1-reason-{reason}"), lane_final(body, 4, 4, 0, 0));
        assert_eq!((l.open_begins, l.raw_valid), (OpenBegins::Unverifiable(0), false), "reason {reason}");
    }
}

// ---- T5: raw verdicts and frames ------------------------------------------------------------------------------------

#[test]
fn t5_single_tick_events_are_valid_raw() {
    let ext = with(std_ext(), 5, spu_events(LANE, 1, 0, &[(500, 0, 1, 0), (500, 0, 2, 0)]));
    let v = sources("t5-single-tick", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.raw_valid, l.paired()), (true, 1));
}

#[test]
fn t5_gpu_frames_none_unresolved_and_bad_none() {
    let ext =
        with(std_ext(), 6, gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 0, 0), (6_000, GPU_DESC, 2, 0, 0, 0)]));
    let v = sources("t5-gpu-none", &ext_file(ext).bytes());
    let g = source(&v, SourceKey::Gpu(RSX_D));
    assert_eq!((g.frames_none, g.raw_valid), (1, true));
    assert!(g.coverage_unknown.iter().any(|c| c.contains("frame NONE")));
    let ext =
        with(std_ext(), 6, gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 1, 42), (6_000, GPU_DESC, 2, 0, 1, 42)]));
    let v = sources("t5-gpu-unresolved", &ext_file(ext).bytes());
    assert_eq!(source(&v, SourceKey::Gpu(RSX_D)).frames_unresolved, 1);
    let ext =
        with(std_ext(), 6, gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 0, 3), (6_000, GPU_DESC, 2, 0, 0, 0)]));
    expect_malformed("t5-gpu-none-frame", &ext_file(ext).bytes(), "frame_owner NONE with frame_id 3");
}

#[test]
fn t5_leading_loss_makes_coverage_unknown_with_clean_events() {
    let mut ext = std_ext();
    ext[5] = loss(LANE, 1, 0, 2);
    ext.insert(6, spu_events(LANE, 1, 2, &[(990, 0, 1, 0), (980, 0, 2, 0)]));
    ext[8] = finals(&[(LANE, 1, 1, 4, 2, 2, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("t5-leading-loss", &ext_file(ext).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!(l.paired(), 1);
    assert!(l.coverage_unknown.iter().any(|c| c == "LOSS"), "whole-capture coverage unknown");
}

// ---- truncation: every cut of a valid extension file is INCOMPLETE or valid, never a panic or a wrong MALFORMED ----

#[test]
fn every_truncation_of_a_valid_extension_file_is_handled() {
    let bytes = ext_file(std_ext()).bytes();
    for n in 0..bytes.len() {
        match load(&bytes[..n]) {
            Outcome::Analysed(a) => assert!(matches!(a.verdict, ps3dk_prof::Verdict::Incomplete(_)), "cut {n}"),
            Outcome::Malformed(m) => panic!("cut {n}: {m}"),
        }
    }
}

// ---- T1b: caps, aggregates and counts that must not drive allocation ------------------------------------------------

#[test]
fn t1_gpu_and_calibration_caps() {
    let g = 37_448usize;
    let mut ev: Vec<(u64, u32, u8, u8, u32, u64)> = (0..g).map(|i| (i as u64, GPU_DESC, 3, 0, 1, 0)).collect();
    let mut ext = std_ext();
    ext[6] = gpu_events(RSX_D, 1, 0, &ev);
    ext[7] = finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, g as u64, g as u64, 0, 0)]);
    let v = sources("t1-gpu-max", &ext_file(ext.clone()).bytes());
    assert_eq!(source(&v, SourceKey::Gpu(RSX_D)).marks(), g as u64);
    ev.push((0, GPU_DESC, 3, 0, 1, 0));
    ext[6] = gpu_events(RSX_D, 1, 0, &ev);
    match load(&ext_file(ext).bytes()) {
        Outcome::Malformed(m) => assert_eq!(m.rule, Rule::ChunkLenOverCap),
        o => panic!("{o:?}"),
    }
    let c = 43_689usize;
    let br: Vec<(u64, u64, u64)> = (0..c).map(|i| ((c - i) as u64, 10 + 2 * i as u64, 11 + 2 * i as u64)).collect();
    let v = sources("t1-cal-max", &ext_file(with(std_ext(), 4, calibration(SPU_D, 1, 0, &br))).bytes());
    assert_eq!(source(&v, SourceKey::Lane(LANE)).brackets, c as u64);
    let mut br = br;
    br.push((0, 1 << 40, 1 << 41));
    match load(&ext_file(with(std_ext(), 4, calibration(SPU_D, 1, 0, &br))).bytes()) {
        Outcome::Malformed(m) => assert_eq!(m.rule, Rule::ChunkLenOverCap),
        o => panic!("{o:?}"),
    }
}

#[test]
fn t1_huge_counts_are_refused_before_any_loop() {
    // Each count field set to u32::MAX over a short payload: refused from the length check alone.
    for (name, ty, prefix, want) in [
        (
            "spu",
            T_SPU,
            {
                let mut b = Buf(Vec::new());
                b.u32(LANE).u32(1).u64(0).u32(u32::MAX);
                b.0
            },
            "SPU_EVENTS n 4294967295 with payload length 20",
        ),
        (
            "gpu",
            T_GPU,
            {
                let mut b = Buf(Vec::new());
                b.u32(RSX_D).u32(1).u64(0).u32(u32::MAX);
                b.0
            },
            "GPU_EVENTS n 4294967295 with payload length 20",
        ),
        (
            "cal",
            T_CAL,
            {
                let mut b = Buf(Vec::new());
                b.u32(SPU_D).u32(1).u64(0).u32(u32::MAX);
                b.0
            },
            "CALIBRATION n 4294967295 with payload length 20",
        ),
        (
            "domain",
            T_DOMAIN,
            {
                let mut b = Buf(Vec::new());
                b.u32(u32::MAX);
                b.0
            },
            "DOMAIN count 4294967295",
        ),
        (
            "epoch",
            T_EPOCH,
            {
                let mut b = Buf(Vec::new());
                b.u32(u32::MAX);
                b.0
            },
            "EPOCH count 4294967295",
        ),
        (
            "images",
            T_IMAGES,
            {
                let mut b = Buf(Vec::new());
                b.u32(u32::MAX);
                b.0
            },
            "SPU_IMAGES count 4294967295",
        ),
        (
            "lanes",
            T_LANES,
            {
                let mut b = Buf(Vec::new());
                b.u32(u32::MAX);
                b.0
            },
            "SPU_LANES count 4294967295",
        ),
        (
            "final",
            T_FINAL,
            {
                let mut b = Buf(Vec::new());
                b.u32(u32::MAX);
                b.0
            },
            "DOMAIN_FINAL count 4294967295",
        ),
    ] {
        let mut ext = std_ext();
        ext.insert(3, chunk(ty, prefix));
        expect_malformed(&format!("t1-huge-{name}"), &ext_file(ext).bytes(), want);
    }
    // A huge desc_count inside an image.
    let mut b = Buf(Vec::new());
    b.u32(1).u32(0).bytes_str(b"b").u32(u32::MAX);
    expect_malformed("t1-huge-desc-count", &ext_file(with(std_ext(), 1, chunk(T_IMAGES, b.0))).bytes(), "desc_count");
}

#[test]
fn t1_aggregate_bounds() {
    // 64 RSX domains fit; the 65th exceeds the per-file bound.
    let first: Vec<(u32, u8, u32)> = (0..64).map(|i| (100 + i, 2u8, 0)).collect();
    let more = domains(&[(200, 2, 0)]);
    expect_malformed("t1-rsx-65", &ext_file(vec![domains(&first), more]).bytes(), "per-file bound");
    // 65 entries in one DOMAIN chunk.
    let many: Vec<(u32, u8, u32)> = (0..65).map(|i| (100 + i, 2u8, 0)).collect();
    expect_malformed("t1-domain-chunk-65", &ext_file(vec![domains(&many)]).bytes(), "DOMAIN count 65");
    // Depth 64 is over the stack bound.
    let ext = with(std_ext(), 5, spu_events(LANE, 1, 0, &[(990, 0, 1, 64), (980, 0, 2, 64)]));
    expect_malformed("t1-depth-64", &ext_file(ext).bytes(), "depth 64");
}

// ---- the CLI layer: exit 4 only for a TRANSPORT COMPLETE extension file with a source not COMPLETE + VALID ---------

fn cli(bytes: &[u8], tag: &str) -> (Option<i32>, String) {
    let path = std::env::temp_dir().join(format!("ps3dk-prof-ext-{}-{tag}.prof", std::process::id()));
    std::fs::write(&path, bytes).unwrap();
    let out = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).arg("check").arg(&path).output().unwrap();
    let _ = std::fs::remove_file(&path);
    (out.status.code(), String::from_utf8_lossy(&out.stdout).into_owned())
}

#[test]
fn cli_exit_status_by_extension_layer() {
    let (code, out) = cli(&ext_file(std_ext()).bytes(), "valid");
    assert_eq!(code, Some(0), "{out}");
    assert!(out.contains("MAPPING is UNMEASURED for every source"), "{out}");
    assert!(out.contains("lane 10 (domain 2): STRUCTURE COMPLETE | RAW VALIDITY VALID | MAPPING UNMEASURED"), "{out}");
    let mut ext = std_ext();
    ext.insert(6, loss(LANE, 1, 2, 3));
    ext[8] = finals(&[(LANE, 1, 1, 5, 2, 3, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let (code, out) = cli(&ext_file(ext).bytes(), "loss");
    assert_eq!(code, Some(4), "{out}");
    assert!(out.contains("RAW VALIDITY NOT VALID"), "{out}");
    let (code, out) = cli(&ext_file(std_ext()).without_trailer().bytes(), "torn");
    assert_eq!(code, Some(1), "{out}");
    let (code, _) = cli(&v1_scenario().bytes(), "v1");
    assert_eq!(code, Some(0));
}

// ---- the decoded raw view: values, endpoint identities and loss ranges round-trip ----------------------------------

#[test]
fn raw_view_round_trips_records_pairs_and_losses() {
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1)]);
    ext.insert(6, loss(LANE, 1, 2, 1));
    ext.insert(
        7,
        spu_events(LANE, 1, 3, &[(960, 1, 2, 1), (955, 0, 3, 1), (950, 0, 3, 0), (945, 1, 1, 0), (940, 1, 2, 0)]),
    );
    ext[9] = finals(&[(LANE, 1, 1, 8, 7, 1, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let bytes = ext_file(ext).bytes();
    let Outcome::Analysed(a) = load(&bytes) else { panic!("readable") };
    let x = a.capture.ext.as_ref().unwrap();
    assert_eq!(
        x.epochs,
        vec![
            EpochRecord { domain_id: SPU_D, epoch: 1, reason: 1, written_value: 0, tb: 5001 },
            EpochRecord { domain_id: RSX_D, epoch: 1, reason: 1, written_value: 0, tb: 5001 },
        ]
    );
    assert_eq!(
        x.brackets,
        vec![
            Bracket { domain_id: SPU_D, epoch: 1, seq: 0, foreign: 1000, tb0: 10, tb1: 20 },
            Bracket { domain_id: SPU_D, epoch: 1, seq: 1, foreign: 900, tb0: 30, tb1: 40 },
        ]
    );
    let l = source(&x.sources, SourceKey::Lane(LANE));
    let ev =
        |seq, raw, desc, kind, depth| RawEvent { seq, epoch: 1, raw, desc, kind, depth, frame_owner: 0, frame_id: 0 };
    assert_eq!(
        l.raw.events,
        vec![
            ev(0, 990, 0, 1, 0),
            ev(1, 985, 1, 1, 1),
            ev(3, 960, 1, 2, 1),
            ev(4, 955, 0, 3, 1),
            ev(5, 950, 0, 3, 0),
            ev(6, 945, 1, 1, 0),
            ev(7, 940, 1, 2, 0)
        ]
    );
    assert_eq!(l.raw.losses, vec![LossRange { first: 2, count: 1, reason: 1 }]);
    assert_eq!(l.raw.censored, vec![0, 1], "both BEGINs open at the LOSS");
    assert_eq!(l.raw.unpaired, vec![3, 4], "END and depth-1 MARK while UNKNOWN");
    assert_eq!(l.raw.marks, vec![5]);
    assert_eq!(l.raw.pairs, vec![Pair { begin: 6, end: 7, desc: 1, depth: 0 }]);
    assert_eq!(l.raw.open, Some(vec![]));
    let g = source(&x.sources, SourceKey::Gpu(RSX_D));
    assert_eq!(
        g.raw.events,
        vec![
            RawEvent { seq: 0, epoch: 1, raw: 5_000, desc: GPU_DESC, kind: 1, depth: 0, frame_owner: 1, frame_id: 0 },
            RawEvent { seq: 1, epoch: 1, raw: 6_000, desc: GPU_DESC, kind: 2, depth: 0, frame_owner: 1, frame_id: 0 },
        ]
    );
    assert_eq!(g.raw.pairs, vec![Pair { begin: 0, end: 1, desc: GPU_DESC, depth: 0 }]);
}

#[test]
fn raw_view_keeps_open_begins_by_seq_at_final_and_at_end() {
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0), (970, 1, 1, 0)]);
    ext[7] = finals(&[(LANE, 1, 1, 3, 3, 0, 1), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let v = sources("raw-open-final", &ext_file(ext.clone()).bytes());
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.raw.open.clone(), l.open_at_end()), (Some(vec![2]), Some(1)));
    ext.pop();
    let bytes = ext_file(ext).bytes();
    let v = sources("raw-open-end", &bytes);
    let l = source(&v, SourceKey::Lane(LANE));
    assert_eq!((l.open_at_end(), l.open_begins), (Some(1), OpenBegins::NoFinal));
    assert!(l.coverage_unknown.iter().any(|c| c == "1 OPEN at end (no FINAL)"), "{:?}", l.coverage_unknown);
}

// ---- the report layer: render() carries the extension section once; check prints it once -------------------------

fn rendered(bytes: &[u8]) -> String {
    let Outcome::Analysed(a) = load(bytes) else { panic!("readable") };
    ps3dk_prof::report::render(&a)
}

fn cli_cmd(cmd: &str, bytes: &[u8], tag: &str) -> (Option<i32>, String) {
    harvest(bytes);
    let path = std::env::temp_dir().join(format!("ps3dk-prof-ext-{}-{tag}.prof", std::process::id()));
    std::fs::write(&path, bytes).unwrap();
    let out = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).arg(cmd).arg(&path).output().unwrap();
    let _ = std::fs::remove_file(&path);
    (out.status.code(), String::from_utf8_lossy(&out.stdout).into_owned())
}

#[test]
fn render_and_both_cli_commands_show_the_extension_layer_once() {
    let mut ext = std_ext();
    ext.insert(6, loss(LANE, 1, 2, 3));
    ext[8] = finals(&[(LANE, 1, 1, 5, 2, 3, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let bytes = ext_file(ext).bytes();
    let r = rendered(&bytes);
    assert_eq!(r.matches("domain extensions:").count(), 1, "{r}");
    assert_eq!(r.matches("SPU lane 10 (domain 2): STRUCTURE COMPLETE | RAW VALIDITY NOT VALID").count(), 1, "{r}");
    assert!(r.starts_with("verdict: COMPLETE (TRANSPORT COMPLETE)"), "{r}");
    for cmd in ["check", "report"] {
        let (code, out) = cli_cmd(cmd, &bytes, &format!("layer-{cmd}"));
        assert_eq!(code, Some(4), "{cmd}: {out}");
        assert_eq!(out.matches("domain extensions:").count(), 1, "{cmd}: {out}");
        assert_eq!(out.matches("RAW VALIDITY NOT VALID (3 events lost").count(), 1, "{cmd}: {out}");
    }
    let (_, report) = cli_cmd("report", &bytes, "layer-report-bytes");
    assert_eq!(report, r, "report prints render() exactly");
    let v1 = rendered(&v1_scenario().bytes());
    assert!(!v1.contains("domain extensions"), "{v1}");
}

// ---- the boundary matrix: per-file aggregates at max and max + 1 -------------------------------------------------

/// `n` SPU domains (ids 100..), each bound to a lane (ids 1000..), declared and finalized in batches of `batch`.
fn many_lanes(n: u32, batch: usize) -> Vec<Chunk> {
    let doms: Vec<(u32, u8, u32)> = (0..n).map(|i| (100 + i, 1, 1000 + i)).collect();
    let mut ext: Vec<Chunk> = doms.chunks(64).map(domains).collect();
    ext.push(images(0, b"img", 1));
    let ls: Vec<(u32, u32, u32, u32)> = (0..n).map(|i| (1000 + i, 100 + i, 0, 0)).collect();
    for c in ls.chunks(batch) {
        ext.push(lanes(c));
        let f: Vec<(u32, u8, u8, u64, u64, u64, u32)> = c.iter().map(|l| (l.0, 1, 1, 0, 0, 0, 0)).collect();
        ext.push(finals(&f));
    }
    ext
}

#[test]
fn bound_lifetime_spu_domains_and_lanes() {
    let v = sources("b-spu-1024", &ext_file(many_lanes(1024, 64)).bytes());
    assert_eq!(v.len(), 1024);
    assert!(v.iter().all(|s| s.structure == Structure::Complete && s.raw_valid));
    expect_malformed("b-spu-1025", &ext_file(many_lanes(1025, 64)).bytes(), "more domains than the per-file bound");
    // Unreachable by construction, so not manufactured: a lane binds exactly one SPU_DEC domain, so the lifetime lane
    // bound (1024) cannot be exceeded before the SPU domain bound; and 1024 SPU + 64 RSX = 1088, the total bound.
}

#[test]
fn bound_live_lanes() {
    sources("b-live-64-then-1", &ext_file(many_lanes(65, 64)).bytes());
    let doms: Vec<(u32, u8, u32)> = (0..65).map(|i| (100 + i, 1, 1000 + i)).collect();
    let mut ext: Vec<Chunk> = doms.chunks(64).map(domains).collect();
    ext.push(images(0, b"img", 1));
    let ls: Vec<(u32, u32, u32, u32)> = (0..65).map(|i| (1000 + i, 100 + i, 0, 0)).collect();
    ext.push(lanes(&ls[..64]));
    ext.push(lanes(&ls[64..]));
    expect_malformed("b-live-65", &ext_file(ext).bytes(), "more than 64 unfinalized lane incarnations");
}

#[test]
fn bound_rsx_domains_and_a_full_final_chunk() {
    // 64 RSX domains + 64 lanes, all finalized by one 128-entry DOMAIN_FINAL (the per-chunk maximum).
    let rsx: Vec<(u32, u8, u32)> = (0..64).map(|i| (3000 + i, 2, 0)).collect();
    let mut ext = vec![domains(&rsx)];
    let mut spu = many_lanes(64, 64);
    spu.pop();
    ext.extend(spu);
    let mut f: Vec<(u32, u8, u8, u64, u64, u64, u32)> = (0..64).map(|i| (1000 + i, 1, 1, 0, 0, 0, 0)).collect();
    f.extend((0..64).map(|i| (3000 + i, 2, 1, 0, 0, 0, 0)));
    ext.push(finals(&f));
    assert_eq!(sources("b-final-128", &ext_file(ext).bytes()).len(), 128);
    expect_malformed(
        "t1-rsx-65-named",
        &ext_file(vec![domains(&rsx), domains(&[(4000, 2, 0)])]).bytes(),
        "more domains than the per-file bound",
    );
}

#[test]
fn bound_images_and_descriptors() {
    let mut ext = std_ext();
    for i in 1..32 {
        ext.insert(2, images(i, b"b", 1));
    }
    let bytes = ext_file(ext.clone()).bytes();
    let Outcome::Analysed(a) = load(&bytes) else { panic!("32 images") };
    assert_eq!(a.capture.ext.as_ref().unwrap().images.len(), 32);
    ext.insert(2, images(32, b"b", 1));
    expect_malformed("b-images-33", &ext_file(ext).bytes(), "more images than the per-file bound");
    let bytes = ext_file(with(std_ext(), 1, images(0, b"b", 4096))).bytes();
    let Outcome::Analysed(a) = load(&bytes) else { panic!("4096 descriptors") };
    assert_eq!(a.capture.ext.as_ref().unwrap().images[&0].descriptors.len(), 4096);
    expect_malformed("b-descs-4097", &ext_file(with(std_ext(), 1, images(0, b"b", 4097))).bytes(), "desc_count 4097");
}

/// The standard chunks with `extra` placed after the events and before the DOMAIN_FINAL.
fn before_final(mut ext: Vec<Chunk>, extra: Vec<Chunk>, fin: Chunk) -> Vec<Chunk> {
    ext.pop();
    ext.extend(extra);
    ext.push(fin);
    ext
}

#[test]
fn bound_epochs_per_file() {
    // std_ext declares 2 epochs; RSX epochs 2..=4095 bring the file to 4096, in EPOCH chunks of 256 (the chunk max).
    let fin = finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let extra = |last: u32| -> Vec<Chunk> {
        let e: Vec<(u32, u32, u8, u64)> = (2..=last).map(|e| (RSX_D, e, 5, 0)).collect();
        e.chunks(256).map(epochs).collect()
    };
    let bytes = ext_file(before_final(std_ext(), extra(4095), fin.clone())).bytes();
    let Outcome::Analysed(a) = load(&bytes) else { panic!("4096 epochs") };
    assert_eq!(a.capture.ext.as_ref().unwrap().epochs.len(), 4096);
    expect_malformed(
        "b-epochs-4097",
        &ext_file(before_final(std_ext(), extra(4096), fin)).bytes(),
        "more epochs than the per-file bound",
    );
}

fn bracket_run(start_tb: u64, n: usize) -> Vec<(u64, u64, u64)> {
    (0..n as u64).map(|i| (1000, start_tb + 2 * i, start_tb + 2 * i + 1)).collect()
}

#[test]
fn bound_brackets_per_epoch() {
    let a = calibration(SPU_D, 1, 0, &bracket_run(10, 43_689));
    let b = calibration(SPU_D, 1, 43_689, &bracket_run(1_000_000, 21_847));
    let mut ext = std_ext();
    ext.splice(4..5, [a.clone(), b.clone()]);
    let v = sources("b-brackets-65536", &ext_file(ext.clone()).bytes());
    assert_eq!(source(&v, SourceKey::Lane(LANE)).brackets, 65_536);
    ext.insert(6, calibration(SPU_D, 1, 65_536, &bracket_run(2_000_000, 1)));
    expect_malformed("b-brackets-65537", &ext_file(ext).bytes(), "epoch 1: more than 65536 brackets");
    // Unreachable: "CALIBRATION bracket sequence overflows" needs first + n > u64::MAX, but first must equal the next
    // bracket number, which never exceeds 65536.
}

#[test]
fn bound_brackets_per_file() {
    let fin = finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let run = |epochs_n: u32| -> Vec<Chunk> {
        let mut out = Vec::new();
        for e in 1..=epochs_n {
            if e > 1 {
                out.push(epochs(&[(RSX_D, e, 5, 0)]));
            }
            if e <= 4 {
                out.push(calibration(RSX_D, e, 0, &bracket_run(10, 43_689)));
                out.push(calibration(RSX_D, e, 43_689, &bracket_run(1_000_000, 21_847)));
            } else {
                out.push(calibration(RSX_D, e, 0, &bracket_run(10, 1)));
            }
        }
        out
    };
    let mut base = std_ext();
    base.remove(4); // no SPU brackets: the RSX domain carries all of them
    let bytes = ext_file(before_final(base.clone(), run(4), fin.clone())).bytes();
    let Outcome::Analysed(a) = load(&bytes) else { panic!("262144 brackets") };
    assert_eq!(a.capture.ext.as_ref().unwrap().brackets.len(), 262_144);
    expect_malformed(
        "b-brackets-262145",
        &ext_file(before_final(base, run(5), fin)).bytes(),
        "more calibration brackets than the per-file bound",
    );
}

#[test]
fn bound_loss_chunks() {
    let losses = |n: u64| -> Vec<Chunk> { (0..n).map(|i| loss(RSX_D, 2, 2 + i, 1)).collect() };
    let fin = |n: u64| finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, 2 + n, 2, n, 0)]);
    let v = sources("b-loss-65536", &ext_file(before_final(std_ext(), losses(65_536), fin(65_536))).bytes());
    assert_eq!(source(&v, SourceKey::Gpu(RSX_D)).raw.losses.len(), 65_536);
    let bytes = ext_file(before_final(std_ext(), losses(65_537), fin(65_537))).bytes();
    expect_malformed("b-loss-65537", &bytes, "more LOSS chunks than the per-file bound");
    // Unreachable: "lost total overflows" - lost never exceeds the source's next sequence number, whose overflow the
    // tiling check refuses first (t2_gap_and_overlap_are_malformed, "sequence overflows").
}

#[test]
fn bound_per_chunk_counts() {
    for (name, ty, count, want) in [
        ("domain-0", T_DOMAIN, 0u32, "DOMAIN count 0"),
        ("epoch-257", T_EPOCH, 257, "EPOCH count 257"),
        ("images-33", T_IMAGES, 33, "SPU_IMAGES count 33"),
        ("lanes-65", T_LANES, 65, "SPU_LANES count 65"),
        ("final-129", T_FINAL, 129, "DOMAIN_FINAL count 129"),
    ] {
        let mut ext = std_ext();
        ext.insert(3, chunk(ty, count.to_le_bytes().to_vec()));
        expect_malformed(&format!("b-chunk-{name}"), &ext_file(ext).bytes(), want);
    }
    // 32 images in one chunk (the chunk maximum).
    let mut b = Buf(Vec::new());
    b.u32(32);
    for i in 0..32 {
        b.u32(i).bytes_str(b"b").u32(1).u32(0).u32(0).u32(1).str("z").str("f");
    }
    let bytes = ext_file(with(std_ext(), 1, chunk(T_IMAGES, b.0))).bytes();
    let Outcome::Analysed(a) = load(&bytes) else { panic!("32-image chunk") };
    assert_eq!(a.capture.ext.as_ref().unwrap().images.len(), 32);
}

// ---- reserved fields, enums, final states and data after FINAL -------------------------------------------------

#[test]
fn reserved_and_enum_families() {
    let mut d = domains(&[(SPU_D, 1, LANE)]);
    d.payload[11] = 1; // the reserved byte after direction
    expect_malformed("r-domain-r0", &ext_file(vec![d]).bytes(), "DOMAIN 2: reserved field nonzero");
    expect_malformed(
        "r-domain-id-65536",
        &ext_file(vec![domains(&[(65_536, 2, 0)])]).bytes(),
        "DOMAIN id 65536 out of 2..65535",
    );
    expect_malformed(
        "r-spu-ref0",
        &ext_file(vec![domains(&[(SPU_D, 1, 0)])]).bytes(),
        "SPU_DEC domain 2: source_ref 0",
    );
    for byte in 0..3 {
        let mut e = epochs(&[(SPU_D, 1, 1, 0), (RSX_D, 1, 1, 0)]);
        e.payload[13 + byte] = 1;
        expect_malformed(
            &format!("r-epoch-r{byte}"),
            &ext_file(with(std_ext(), 3, e)).bytes(),
            "EPOCH 2/1: reserved nonzero",
        );
    }
    let mut g = gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 0, 1, 0), (6_000, GPU_DESC, 2, 0, 1, 0)]);
    g.payload[20 + 14] = 1;
    expect_malformed("r-gpu-reserved", &ext_file(with(std_ext(), 6, g)).bytes(), "GPU domain 3: event reserved 1");
    for kind in [0u8, 4] {
        let ext = with(std_ext(), 5, spu_events(LANE, 1, 0, &[(990, 0, kind, 0), (980, 0, 2, 0)]));
        expect_malformed(
            &format!("r-spu-kind{kind}"),
            &ext_file(ext).bytes(),
            &format!("lane 10: event kind {kind} depth 0"),
        );
        let ext = with(
            std_ext(),
            6,
            gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, kind, 0, 1, 0), (6_000, GPU_DESC, 2, 0, 1, 0)]),
        );
        expect_malformed(
            &format!("r-gpu-kind{kind}"),
            &ext_file(ext).bytes(),
            &format!("GPU domain 3: event kind {kind} depth 0"),
        );
    }
    let ext =
        with(std_ext(), 6, gpu_events(RSX_D, 1, 0, &[(5_000, GPU_DESC, 1, 64, 1, 0), (6_000, GPU_DESC, 2, 0, 1, 0)]));
    expect_malformed("r-gpu-depth64", &ext_file(ext).bytes(), "GPU domain 3: event kind 1 depth 64");
    for (name, c, want) in [
        (
            "loss-r0",
            {
                let mut l = loss(LANE, 1, 2, 1);
                l.payload[6] = 1;
                l
            },
            "LOSS reserved nonzero",
        ),
        (
            "loss-r1",
            {
                let mut l = loss(LANE, 1, 2, 1);
                l.payload[24] = 1;
                l
            },
            "LOSS reserved nonzero",
        ),
        (
            "loss-reason0",
            {
                let mut l = loss(LANE, 1, 2, 1);
                l.payload[5] = 0;
                l
            },
            "LOSS reason 0 count 1",
        ),
        (
            "loss-reason5",
            {
                let mut l = loss(LANE, 1, 2, 1);
                l.payload[5] = 5;
                l
            },
            "LOSS reason 5 count 1",
        ),
        ("loss-count0", loss(LANE, 1, 2, 0), "LOSS reason 1 count 0"),
        ("loss-kind3", loss(LANE, 3, 2, 1), "source_kind 3"),
        ("loss-gpu-not-rsx", loss(SPU_D, 2, 2, 1), "LOSS names GPU domain 2, not a declared RSX domain"),
    ] {
        let mut ext = std_ext();
        ext.insert(6, c);
        expect_malformed(&format!("r-{name}"), &ext_file(ext).bytes(), want);
    }
    for (name, entry, want) in [
        ("final-r", None, "DOMAIN_FINAL 10: reserved 1 state 1"),
        ("final-state0", Some((LANE, 1u8, 0u8)), "DOMAIN_FINAL 10: reserved 0 state 0"),
        ("final-state4", Some((LANE, 1, 4)), "DOMAIN_FINAL 10: reserved 0 state 4"),
        ("final-kind3", Some((LANE, 3, 1)), "source_kind 3"),
    ] {
        let c = match entry {
            None => {
                let mut f = finals(&[(LANE, 1, 1, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
                f.payload[10] = 1;
                f
            }
            Some((s, k, st)) => finals(&[(s, k, st, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]),
        };
        expect_malformed(&format!("r-{name}"), &ext_file(with(std_ext(), 7, c)).bytes(), want);
    }
    for (name, id, want) in [
        ("build-id-0", &b""[..], "image 0: build_id of 0 bytes"),
        ("build-id-65", &[7u8; 65][..], "image 0: build_id of 65 bytes"),
    ] {
        expect_malformed(&format!("r-{name}"), &ext_file(with(std_ext(), 1, images(0, id, 2))).bytes(), want);
    }
    sources("r-build-id-64", &ext_file(with(std_ext(), 1, images(0, &[7u8; 64], 2))).bytes());
    let mut b = Buf(Vec::new());
    b.u32(1).u32(0).bytes_str(b"b").u32(1).u32(0).u32(0).u32(1).str("").str("f");
    expect_malformed(
        "r-desc-empty-name",
        &ext_file(with(std_ext(), 1, chunk(T_IMAGES, b.0))).bytes(),
        "image 0 descriptor 0: empty name",
    );
}

#[test]
fn abandoned_and_overflowed_are_incomplete_sources() {
    for (state, word) in [(2u8, "ABANDONED"), (3, "OVERFLOWED")] {
        let ext = with(std_ext(), 7, finals(&[(LANE, 1, state, 2, 2, 0, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]));
        let bytes = ext_file(ext).bytes();
        let v = sources(&format!("s-{word}"), &bytes);
        let l = source(&v, SourceKey::Lane(LANE));
        assert_eq!((l.structure, l.raw_valid, l.final_state), (Structure::Incomplete, false, Some(state)));
        assert_eq!(l.incomplete, vec![word.to_string()]);
        assert_eq!(cli_cmd("check", &bytes, word).0, Some(4));
    }
}

#[test]
fn epoch_calibration_and_loss_after_final_are_malformed() {
    for (name, c) in [
        ("epoch", epochs(&[(RSX_D, 2, 5, 0)])),
        ("calibration", calibration(SPU_D, 1, 2, &[(800, 50, 60)])),
        ("loss", loss(RSX_D, 2, 2, 1)),
    ] {
        let mut ext = std_ext();
        ext.push(c);
        expect_malformed(&format!("af-{name}"), &ext_file(ext).bytes(), "data after its DOMAIN_FINAL");
    }
}

// ---- the decrementer observed-variation classification (design p3.2 dec-write r2 section 3; review 02:07/02:08) ----

use ps3dk_prof::ext::{counter_variation, CounterVariation, EpochVariation, ExtCapture};
use CounterVariation::{Insufficient, Moving, Static};

const M: u8 = 3; // MARK: an observation that makes no pairing claim

/// One SPU lane only: epoch 1 declared (reason 1, written_value 0), then `body`, then its FINAL (state, next, events,
/// lost).
fn lane_only(body: Vec<Chunk>, fin: (u8, u64, u64, u64)) -> Vec<Chunk> {
    let (state, next, events, lost) = fin;
    let mut v = vec![
        domains(&[(SPU_D, 1, LANE)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0)]),
    ];
    v.extend(body);
    v.push(finals(&[(LANE, 1, state, next, events, lost, 0)]));
    v
}

fn capture(name: &str, ext: Vec<Chunk>) -> ExtCapture {
    let bytes = ext_file(ext).bytes();
    dump(name, &bytes);
    match load(&bytes) {
        Outcome::Analysed(a) => a.capture.ext.expect("extension capture"),
        Outcome::Malformed(m) => panic!("{name}: expected a readable file, got {m}"),
    }
}

fn variation(name: &str, ext: Vec<Chunk>) -> (Vec<EpochVariation>, SourceReport) {
    let x = capture(name, ext);
    let s = x.sources.iter().find(|s| s.key == SourceKey::Lane(LANE)).expect("lane").clone();
    (counter_variation(&x, &s).expect("an SPU lane is classified"), s)
}

fn epv(epoch: u32, events: u64, brackets: u64, state: CounterVariation, span: Option<u64>) -> EpochVariation {
    EpochVariation { epoch, events, brackets, state, loss_records_in_event_span: span }
}

#[test]
fn cv_design_cases() {
    // 0 observations: the epoch is declared, nothing observed
    let (v, _) = variation("cv-0", lane_only(vec![], (1, 0, 0, 0)));
    assert_eq!(v, vec![epv(1, 0, 0, Insufficient, None)]);
    // 1 observation
    let (v, _) = variation("cv-1", lane_only(vec![spu_events(LANE, 1, 0, &[(990, 0, M, 0)])], (1, 1, 1, 0)));
    assert_eq!(v, vec![epv(1, 1, 0, Insufficient, None)]);
    // 2 equal / 2 unequal
    let two_eq = vec![spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)])];
    let (v, _) = variation("cv-2eq", lane_only(two_eq.clone(), (1, 2, 2, 0)));
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(0))]);
    let two_ne = vec![spu_events(LANE, 1, 0, &[(990, 0, M, 0), (980, 0, M, 0)])];
    let (v, _) = variation("cv-2ne", lane_only(two_ne, (1, 2, 2, 0)));
    assert_eq!(v, vec![epv(1, 2, 0, Moving, Some(0))]);
    // a wrap is just variation
    let wrap = vec![spu_events(LANE, 1, 0, &[(1, 0, M, 0), (0xFFFF_FFFE, 0, M, 0)])];
    let (v, _) = variation("cv-wrap", lane_only(wrap, (1, 2, 2, 0)));
    assert_eq!(v[0].state, Moving);
    // the EPOCH written_value (epoch 2, the dec_write) differs from every (equal) read: it is not an observation
    let body = vec![
        two_eq[0].clone(),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 2, &[(990, 0, M, 0), (990, 0, M, 0)]),
    ];
    let (v, _) = variation("cv-written", lane_only(body, (1, 4, 4, 0)));
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(0)), epv(2, 2, 0, Static, Some(0))]);
}

#[test]
fn cv_equal_values_with_a_loss_between_them_and_unchanged_qualifiers() {
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, M, 0)]),
        loss(LANE, 1, 1, 2),
        spu_events(LANE, 1, 3, &[(990, 0, M, 0)]),
    ];
    let (v, s) = variation("cv-loss-between", lane_only(body, (1, 4, 2, 2)));
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(1))], "1 LOSS record (2 lost events) inside the event span");
    assert_eq!((s.structure, s.raw_valid, s.lost_total), (Structure::Complete, false, 2));
    assert!(s.coverage_unknown.iter().any(|c| c == "LOSS"), "{:?}", s.coverage_unknown);
}

#[test]
fn cv_half_open_loss_containment() {
    // leading LOSS [0,2) before the span; [3,5) ends exactly at the last event seq b = 5 (contained: its last lost
    // seq is b-1); trailing LOSS [6,7) after the span
    let body = vec![
        loss(LANE, 1, 0, 2),
        spu_events(LANE, 1, 2, &[(990, 0, M, 0)]),
        loss(LANE, 1, 3, 2),
        spu_events(LANE, 1, 5, &[(990, 0, M, 0)]),
        loss(LANE, 1, 6, 1),
    ];
    let (v, _) = variation("cv-halfopen", lane_only(body, (1, 7, 2, 5)));
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(1))]);
}

#[test]
fn cv_two_epochs_independent_with_an_epoch_boundary_loss() {
    // epoch 1 seqs 0,1 equal; LOSS at seq 2 between the epochs (inside neither event span); epoch 2 seqs 3,4 unequal
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)]),
        loss(LANE, 1, 2, 1),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 3, &[(0xFFFF_0000, 0, M, 0), (0xFFFE_0000, 0, M, 0)]),
    ];
    let (v, s) = variation("cv-two-epochs", lane_only(body, (1, 5, 4, 1)));
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(0)), epv(2, 2, 0, Moving, Some(0))]);
    assert!(!s.raw_valid && s.invalid.iter().any(|r| r == "open_begins not verified"), "{:?}", s.invalid);
}

#[test]
fn cv_epoch_2_transition_without_loss_is_classified_with_its_validity_from_the_final() {
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 2, &[(0xFFFF_FFFE, 0, M, 0), (0xFFFF_E930, 0, M, 0)]),
    ];
    // a depth-0 transition with the true count in the FINAL: valid under the depth-zero rule
    let (v, s) = variation("cv-epoch2", lane_only(body.clone(), (1, 4, 4, 0)));
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(0)), epv(2, 2, 0, Moving, Some(0))]);
    assert_eq!((s.structure, s.raw_valid), (Structure::Complete, true));
    // the same with the older runtime's UINT32_MAX placeholder: classified the same, its invalidity kept
    let (v, s) = variation("cv-epoch2-placeholder", lane_final(body, 4, 4, 0, u32::MAX));
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(0)), epv(2, 2, 0, Moving, Some(0))]);
    assert_eq!((s.structure, s.raw_valid), (Structure::Complete, false));
}

#[test]
fn cv_brackets_alone_and_event_versus_bracket() {
    let body = vec![calibration(SPU_D, 1, 0, &[(500, 10, 20), (500, 30, 40)])];
    let (v, _) = variation("cv-brackets", lane_only(body, (1, 0, 0, 0)));
    assert_eq!(v, vec![epv(1, 0, 2, Static, None)], "brackets classify; the event span stays NOT AVAILABLE");
    let body = vec![calibration(SPU_D, 1, 0, &[(1000, 10, 20)]), spu_events(LANE, 1, 0, &[(990, 0, M, 0)])];
    let (v, _) = variation("cv-ev-br", lane_only(body, (1, 1, 1, 0)));
    assert_eq!(v, vec![epv(1, 1, 1, Moving, None)]);
}

#[test]
fn cv_pairs_and_marks_do_not_add_observations() {
    let body = vec![spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (990, 0, 2, 0), (990, 0, M, 0)])];
    let (v, s) = variation("cv-pairs", lane_only(body, (1, 3, 3, 0)));
    assert_eq!((s.paired(), s.marks()), (1, 1));
    assert_eq!(v, vec![epv(1, 3, 0, Static, Some(0))], "3 stored events, counted once each");
}

#[test]
fn cv_incomplete_sources_are_classified_with_their_qualifiers() {
    for (state, word) in [(2u8, "ABANDONED"), (3, "OVERFLOWED")] {
        let body = vec![spu_events(LANE, 1, 0, &[(990, 0, M, 0), (980, 0, M, 0)])];
        let (v, s) = variation(&format!("cv-{word}"), lane_only(body, (state, 2, 2, 0)));
        assert_eq!(v, vec![epv(1, 2, 0, Moving, Some(0))], "{word}");
        assert_eq!((s.structure, s.raw_valid), (Structure::Incomplete, false), "{word}");
        assert_eq!(s.incomplete, vec![word.to_string()]);
    }
    // no FINAL at all
    let mut ext = lane_only(vec![spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)])], (1, 2, 2, 0));
    ext.pop();
    let (v, s) = variation("cv-nofinal", ext);
    assert_eq!(v, vec![epv(1, 2, 0, Static, Some(0))]);
    assert_eq!(s.open_begins, OpenBegins::NoFinal);
}

#[test]
fn cv_protocol_error_source_classifies_only_its_recorded_values() {
    // the t4 protocol-error shape: END desc 1 vs open desc 0, then a MARK; all three events stay decoded
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 2, 0)]),
        spu_events(LANE, 1, 2, &[(970, 0, M, 0)]),
    ];
    let (v, s) = variation("cv-protocol", lane_only(body, (1, 3, 3, 0)));
    assert_eq!(v, vec![epv(1, 3, 0, Moving, Some(0))]);
    assert!(s.protocol_error.is_some() && !s.raw_valid);
    assert_eq!(s.raw.open, None, "the pairing view stays UNKNOWN; only equality/variation is classified");
}

#[test]
fn cv_domain_isolation_across_lanes() {
    // lane 10 / domain 2 equal events; lane 11 / domain 4 unequal brackets: neither sees the other's observations
    let ext = vec![
        domains(&[(SPU_D, 1, LANE), (4, 1, 11)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1), (11, 4, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0), (4, 1, 1, 0)]),
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)]),
        calibration(4, 1, 0, &[(1, 10, 20), (2, 30, 40)]),
        finals(&[(LANE, 1, 1, 2, 2, 0, 0), (11, 1, 1, 0, 0, 0, 0)]),
    ];
    let x = capture("cv-isolation", ext);
    let a = x.sources.iter().find(|s| s.key == SourceKey::Lane(LANE)).unwrap();
    let b = x.sources.iter().find(|s| s.key == SourceKey::Lane(11)).unwrap();
    assert_eq!(counter_variation(&x, a).unwrap(), vec![epv(1, 2, 0, Static, Some(0))]);
    assert_eq!(counter_variation(&x, b).unwrap(), vec![epv(1, 0, 2, Moving, None)]);
}

#[test]
fn cv_gpu_sources_are_not_classified() {
    let x = capture("cv-gpu", std_ext());
    let g = x.sources.iter().find(|s| s.key == SourceKey::Gpu(RSX_D)).unwrap();
    assert_eq!(counter_variation(&x, g), None);
}

#[test]
fn cv_duplicate_bracket_identity_is_still_malformed() {
    // a repeated bracket seq is rejected by the reader (never deduplicated by the classification); the duplicate
    // EVENT identity case is t2_gap_and_overlap_are_malformed ("events start at 1, want 2")
    let mut ext = std_ext();
    ext.insert(5, calibration(SPU_D, 1, 1, &[(800, 50, 60)]));
    expect_malformed("cv-dup-bracket", &ext_file(ext).bytes(), "brackets start at 1, want 2");
}

#[test]
fn cv_report_lines() {
    let body = vec![
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        spu_events(LANE, 2, 2, &[(5, 0, M, 0), (4, 0, M, 0)]),
        epochs(&[(SPU_D, 3, 2, 0xFFFF_FFFF)]),
    ];
    // the older runtime's UINT32_MAX FINAL keeps open_begins unverifiable across the depth-0 transitions
    let r = rendered(&ext_file(lane_final(body, 4, 4, 0, u32::MAX)).bytes());
    let q = " | source RAW VALIDITY is NOT VALID: open_begins not verified";
    let tail = "| order and raw endpoints only (no durations)";
    for line in [
        format!(
            "    epoch 1: COUNTER_STATIC (all observed values equal; says nothing about unobserved intervals) | \
             observations: 2 events + 0 brackets | LOSS records inside the observed event-sequence span: 0 {tail}{q}"
        ),
        format!(
            "    epoch 2: COUNTER_MOVING (variation observed; not proof of continuous counting) | observations: 2 \
             events + 0 brackets | LOSS records inside the observed event-sequence span: 0 {tail}{q}"
        ),
        format!(
            "    epoch 3: INSUFFICIENT (fewer than 2 observations) | observations: 0 events + 0 brackets | LOSS \
             records inside the observed event-sequence span: NOT AVAILABLE (fewer than 2 events) {tail}{q}"
        ),
    ] {
        assert_eq!(r.lines().filter(|l| *l == line).count(), 1, "missing {line:?} in\n{r}");
    }
    assert!(r.contains("MAPPING UNMEASURED"), "{r}");
    let g = rendered(&ext_file(std_ext()).bytes());
    assert_eq!(g.matches("    epoch ").count(), 1, "one epoch line for the lane, none for the GPU domain:\n{g}");
}

// ---- conditional affine bounds over a decoded capture ---------------------------------------------------------

use ps3dk_prof::clockmap::{
    lines as clockmap_lines, map_capture, solve, Budgets, CaptureMap, Coordinate, EpochState, Limit, Meter, Obs,
    Premises, SPU_DEC_TOP,
};

const NOWRAP: Premises = Premises { spu_nowrap: true };

/// std_ext plus two RSX brackets: b in [(400-110)/3000, (410-100)/3000] = [29/300, 31/300]; the SPU brackets under
/// W32 sit at f = TOP-1000 and TOP-900: b in [(30-20)/100, (40-10)/100] = [1/10, 3/10].  Every event lies between
/// its epoch's brackets, at 1/3 and 2/3 (RSX) or 1/10 and 2/10 (SPU) of the way: each width is exactly 10.
fn cm_ext() -> Vec<Chunk> {
    let mut ext = std_ext();
    ext.insert(5, calibration(RSX_D, 1, 0, &[(4000, 100, 110), (7000, 400, 410)]));
    ext
}

fn state(c: &CaptureMap, key: SourceKey, epoch: u32) -> &EpochState {
    &c.epochs.iter().find(|e| e.source == key && e.epoch == epoch).unwrap_or_else(|| panic!("{key} {epoch}")).state
}

#[test]
fn cm_default_spu_is_ambiguous_and_rsx_is_mapped() {
    let x = capture("cm-default", cm_ext());
    let c = map_capture(&x, Premises::default(), &Budgets::default());
    let spu = c.epochs.iter().find(|e| e.source == SourceKey::Lane(LANE)).unwrap();
    assert_eq!((spu.coordinate, spu.brackets, &spu.state), (None, 2, &EpochState::AmbiguousLift));
    let gpu = c.epochs.iter().find(|e| e.source == SourceKey::Gpu(RSX_D)).unwrap();
    assert_eq!(gpu.coordinate, Some(Coordinate::R64));
    let EpochState::Compatible(s) = &gpu.state else { panic!("{:?}", gpu.state) };
    assert_eq!(
        (s.slope_lo.to_string(), s.slope_lo_open, s.slope_hi.as_ref().map(|z| z.to_string())),
        ("29/300".to_string(), false, Some("31/300".to_string()))
    );
    assert_eq!((s.events, s.interpolated, s.extrapolated, s.bounded, s.open_bound), (2, 2, 0, 2, 0));
    assert_eq!(
        (s.width_max.as_ref().unwrap().to_string(), s.width_median.as_ref().unwrap().to_string()),
        ("10".to_string(), "10".to_string())
    );
    let l = clockmap_lines(&x, &c);
    let spu_line = format!(
        "  SPU lane {LANE} (domain {SPU_D}) epoch 1: AMBIGUOUS-LIFT (no SPU coordinate premise selected; the \
         capture does not determine the decrementer's wrap count) | premises none applied | brackets 2"
    );
    let gpu_line = format!(
        "  GPU domain {RSX_D} (domain {RSX_D}) epoch 1: COMPATIBLE | premises C,A,S,R64 | brackets 2 distinct_f 2 \
         | slope [29/300, 31/300] | covered f [4000, 7000] | events 2: interpolated 2 extrapolated 0; bounded 2 \
         half-unbounded 0 unbounded 0; open-bound 0 | width max 10 median 10"
    );
    assert!(l.contains(&spu_line), "{l:#?}");
    assert!(l.contains(&gpu_line), "{l:#?}");
    assert!(!l.iter().any(|t| t.contains("SPU lane") && t.contains("W32")), "W32 was not selected: {l:#?}");
}

#[test]
fn cm_spu_under_w32() {
    let x = capture("cm-w32", cm_ext());
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let spu = c.epochs.iter().find(|e| e.source == SourceKey::Lane(LANE)).unwrap();
    assert_eq!(spu.coordinate, Some(Coordinate::W32));
    let EpochState::Compatible(s) = &spu.state else { panic!("{:?}", spu.state) };
    assert_eq!(
        (s.slope_lo.to_string(), s.slope_hi.as_ref().unwrap().to_string()),
        ("1/10".to_string(), "3/10".to_string())
    );
    assert_eq!((s.fmin, s.fmax), (SPU_DEC_TOP - 1000, SPU_DEC_TOP - 900));
    assert_eq!(s.width_median.as_ref().unwrap().to_string(), "10");
    assert!(clockmap_lines(&x, &c).iter().any(|t| t.contains("SPU lane") && t.contains("premises C,A,S,W32")));
}

#[test]
fn cm_no_rsx_brackets_is_no_observations() {
    let x = capture("cm-noobs", std_ext());
    let c = map_capture(&x, Premises::default(), &Budgets::default());
    assert_eq!(state(&c, SourceKey::Gpu(RSX_D), 1), &EpochState::NoObservations);
}

#[test]
fn cm_cache_is_scoped_to_the_epoch() {
    // The same raw value 950 in two epochs with different brackets: f is equal, the models are not.  Epoch 1 width
    // at the midpoint: (20+40)/2 - (10+30)/2 = 10; epoch 2: (120+140)/2 - (100+130)/2 = 15.
    let body = vec![
        calibration(SPU_D, 1, 0, &[(1000, 10, 20), (900, 30, 40)]),
        spu_events(LANE, 1, 0, &[(950, 0, M, 0), (950, 0, M, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        calibration(SPU_D, 2, 0, &[(1000, 100, 120), (900, 130, 140)]),
        spu_events(LANE, 2, 2, &[(950, 0, M, 0), (950, 0, M, 0)]),
    ];
    let x = capture("cm-cache", lane_only(body, (1, 4, 4, 0)));
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let width = |e| match state(&c, SourceKey::Lane(LANE), e) {
        EpochState::Compatible(s) => s.width_median.as_ref().unwrap().to_string(),
        other => panic!("{other:?}"),
    };
    assert_eq!((width(1), width(2)), ("10".to_string(), "15".to_string()));
}

#[test]
fn cm_capture_budget_boundaries_are_exact_and_atomic() {
    let x = capture("cm-budget", cm_ext());
    let full = map_capture(&x, NOWRAP, &Budgets::default());
    assert_eq!(full.exhausted_at, None);
    let need = full.used;
    let with = |capture| map_capture(&x, NOWRAP, &Budgets { capture, ..Budgets::default() });
    // Exactly enough: the identical result.
    let exact = with(need);
    assert_eq!((exact.epochs.clone(), exact.exhausted_at), (full.epochs.clone(), None));
    // One under: the LAST mapped epoch (the GPU domain) runs out; the SPU result is complete and unchanged.
    let short = with(need - 1);
    assert_eq!(short.exhausted_at, Some((SourceKey::Gpu(RSX_D), 1)));
    assert_eq!(state(&short, SourceKey::Gpu(RSX_D), 1), &EpochState::Unsupported(Limit::Capture));
    assert_eq!(state(&short, SourceKey::Lane(LANE), 1), state(&full, SourceKey::Lane(LANE), 1));
    // The SPU source pays its undeclared-epoch scan (2 events), then the solve completes and the distinct-coordinate
    // pass (2 more) runs out: no partial result, and every later mapping-eligible epoch is UNSUPPORTED too.
    let obs: Vec<Obs> = [(1000u64, 10u64, 20u64), (900, 30, 40)]
        .iter()
        .map(|&(f, lo, hi)| Obs { f: SPU_DEC_TOP - i128::from(f), lo: i128::from(lo), hi: i128::from(hi) })
        .collect();
    let mut m = Meter::unlimited();
    solve(&mut m, &obs).unwrap();
    let mid = with(2 + m.used() + 1);
    assert_eq!(mid.exhausted_at, Some((SourceKey::Lane(LANE), 1)));
    assert_eq!(state(&mid, SourceKey::Lane(LANE), 1), &EpochState::Unsupported(Limit::Capture));
    assert_eq!(state(&mid, SourceKey::Gpu(RSX_D), 1), &EpochState::Unsupported(Limit::Capture));
}

#[test]
fn cm_zero_budget_keeps_true_bracket_counts() {
    let x = capture("cm-zero", cm_ext());
    let zero = Budgets { capture: 0, ..Budgets::default() };
    let w = map_capture(&x, NOWRAP, &zero);
    assert_eq!(w.exhausted_at, Some((SourceKey::Lane(LANE), 1)));
    for key in [SourceKey::Lane(LANE), SourceKey::Gpu(RSX_D)] {
        let e = w.epochs.iter().find(|e| e.source == key).unwrap();
        assert_eq!((e.brackets, &e.state), (2, &EpochState::Unsupported(Limit::Capture)), "{key}");
    }
    let d = map_capture(&x, Premises::default(), &zero);
    let spu = d.epochs.iter().find(|e| e.source == SourceKey::Lane(LANE)).unwrap();
    assert_eq!((spu.brackets, &spu.state), (2, &EpochState::AmbiguousLift));
    let gpu = d.epochs.iter().find(|e| e.source == SourceKey::Gpu(RSX_D)).unwrap();
    assert_eq!((gpu.brackets, &gpu.state), (2, &EpochState::Unsupported(Limit::Capture)));
    assert_eq!((d.exhausted_at, d.used), (Some((SourceKey::Gpu(RSX_D), 1)), 0));
    let l = clockmap_lines(&x, &d);
    assert!(l.iter().any(|t| t.contains("AMBIGUOUS-LIFT") && t.ends_with("| brackets 2")), "{l:#?}");
    assert!(l.iter().any(|l| l.starts_with("clockmap: capture budget exhausted at GPU domain")));
}

#[test]
fn cm_ambiguous_only_capture_does_no_work() {
    let body = vec![
        calibration(SPU_D, 1, 0, &[(1000, 10, 20), (900, 30, 40)]),
        spu_events(LANE, 1, 0, &[(950, 0, M, 0), (940, 0, M, 0)]),
    ];
    let x = capture("cm-ambiguous-only", lane_only(body, (1, 2, 2, 0)));
    let c = map_capture(&x, Premises::default(), &Budgets::default());
    assert_eq!(c.used, 0, "no charged work: nothing is mapped");
    assert_eq!(c.epochs.len(), 1);
    assert_eq!((c.epochs[0].brackets, &c.epochs[0].state), (2, &EpochState::AmbiguousLift));
}

#[test]
fn cm_repeated_coordinate_source_is_bounded_by_distinct_values() {
    // 100000 events at one raw value: one distinct coordinate, so a cap of 1 still maps every event.
    let n = 100_000u64;
    let ev = vec![(950u32, 0u16, M, 0u8); n as usize];
    let body = vec![calibration(SPU_D, 1, 0, &[(1000, 10, 20), (900, 30, 40)]), spu_events(LANE, 1, 0, &ev)];
    let x = capture("cm-repeated", lane_only(body, (1, n, n, 0)));
    let c = map_capture(&x, NOWRAP, &Budgets { max_distinct_q: 1, ..Budgets::default() });
    let EpochState::Compatible(s) = state(&c, SourceKey::Lane(LANE), 1) else { panic!("{c:?}") };
    assert_eq!((s.events, s.interpolated, s.width_median.as_ref().unwrap().to_string()), (n, n, "10".to_string()));
}

#[test]
fn cm_distinct_coordinate_cap() {
    let x = capture("cm-qcap", cm_ext());
    let c = map_capture(&x, Premises::default(), &Budgets { max_distinct_q: 1, ..Budgets::default() });
    assert_eq!(state(&c, SourceKey::Gpu(RSX_D), 1), &EpochState::Unsupported(Limit::Coordinates));
}

fn cm_cli(args: &[&str], bytes: Option<&[u8]>, tag: &str) -> (Option<i32>, String) {
    let path = std::env::temp_dir().join(format!("ps3dk-prof-cm-{}-{tag}.prof", std::process::id()));
    let mut argv: Vec<String> = args.iter().map(|s| s.to_string()).collect();
    if let Some(b) = bytes {
        std::fs::write(&path, b).expect("write the capture");
        argv.push(path.to_string_lossy().into_owned());
    }
    let out = std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(&argv).output().expect("run");
    let _ = std::fs::remove_file(&path);
    (out.status.code(), String::from_utf8_lossy(&out.stdout).into_owned())
}

#[test]
fn cm_cli_exit_status_is_the_check_status() {
    let clean = ext_file(cm_ext()).bytes();
    let invalid = {
        let body = vec![
            spu_events(LANE, 1, 0, &[(990, 0, M, 0), (990, 0, M, 0)]),
            epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
            spu_events(LANE, 2, 2, &[(5, 0, M, 0), (4, 0, M, 0)]),
        ];
        // invalid: the older runtime's UINT32_MAX FINAL after a depth-0 transition stays unverifiable
        ext_file(lane_final(body, 4, 4, 0, u32::MAX)).bytes()
    };
    let mut bad = std_ext();
    bad[4] = calibration(SPU_D, 1, 0, &[(1000, 20, 20)]);
    let malformed = ext_file(bad).bytes();
    let v1 = v1_scenario().bytes();
    for (tag, bytes, want) in [("clean", &clean, 0), ("invalid", &invalid, 4), ("malformed", &malformed, 2), ("v1", &v1, 0)]
    {
        let (check, _) = cm_cli(&["check"], Some(bytes), &format!("{tag}-check"));
        let (code, out) = cm_cli(&["clockmap"], Some(bytes), &format!("{tag}-cm"));
        let (code_w, _) = cm_cli(&["clockmap", "--premise", "spu-nowrap"], Some(bytes), &format!("{tag}-cmw"));
        assert_eq!((check, code, code_w), (Some(want), Some(want), Some(want)), "{tag}: {out}");
    }
    let (_, out) = cm_cli(&["clockmap"], Some(&v1), "v1-text");
    assert!(out.contains("clockmap: no domain extensions (nothing to map)"), "{out}");
    let (_, out) = cm_cli(&["clockmap"], Some(&clean), "clean-text");
    assert!(out.starts_with("COMPLETE (TRANSPORT COMPLETE)\nclockmap: CONDITIONAL AFFINE BOUNDS"), "{out}");
    // The report is untouched by the mapping.
    let (_, rep) = cm_cli(&["report"], Some(&clean), "clean-report");
    assert!(rep.contains("MAPPING UNMEASURED") && !rep.contains("clockmap"), "{rep}");
}

#[test]
fn cm_cli_prints_the_source_facts_beside_the_results() {
    let cal = || calibration(SPU_D, 1, 0, &[(1000, 10, 20), (900, 30, 40)]);
    let cases = [
        (
            "transition",
            // the older runtime's UINT32_MAX FINAL after a depth-0 transition: open_begins stays unverifiable
            lane_final(
                vec![
                    cal(),
                    spu_events(LANE, 1, 0, &[(950, 0, M, 0), (950, 0, M, 0)]),
                    epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
                    spu_events(LANE, 2, 2, &[(5, 0, M, 0), (4, 0, M, 0)]),
                ],
                4,
                4,
                0,
                u32::MAX,
            ),
            "open_begins not verified",
        ),
        (
            "loss",
            lane_only(
                vec![
                    cal(),
                    spu_events(LANE, 1, 0, &[(990, 0, M, 0)]),
                    loss(LANE, 1, 1, 1),
                    spu_events(LANE, 1, 2, &[(980, 0, M, 0)]),
                ],
                (1, 3, 2, 1),
            ),
            "1 events lost",
        ),
        (
            "protocol",
            lane_only(
                vec![
                    cal(),
                    spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 2, 0)]),
                    spu_events(LANE, 1, 2, &[(970, 0, M, 0)]),
                ],
                (1, 3, 3, 0),
            ),
            "PROTOCOL ERROR",
        ),
    ];
    for (tag, ext, reason) in cases {
        let bytes = ext_file(ext).bytes();
        let (check_code, check) = cm_cli(&["check"], Some(&bytes), &format!("{tag}-check"));
        let (code, out) = cm_cli(&["clockmap", "--premise", "spu-nowrap"], Some(&bytes), &format!("{tag}-cm"));
        assert_eq!(code, check_code, "{tag}");
        let facts: Vec<&str> =
            check.lines().filter(|l| l.starts_with("  SPU lane") || l.starts_with("    events")).collect();
        assert_eq!(facts.len(), 2, "{tag}: {check}");
        assert!(facts[0].contains(reason), "{tag}: {}", facts[0]);
        let got: Vec<&str> = out.lines().collect();
        let at = got.iter().position(|l| *l == facts[0]).unwrap_or_else(|| panic!("{tag}: no fact line in: {out}"));
        assert_eq!(got[at + 1], facts[1], "{tag}");
        assert!(got[at + 2].starts_with(&format!("  SPU lane {LANE} (domain {SPU_D}) epoch 1: COMPATIBLE")), "{tag}: {out}");
    }
}

#[test]
fn cm_cli_premise_parsing() {
    let clean = ext_file(cm_ext()).bytes();
    for (tag, args) in [
        ("unknown", vec!["clockmap", "--premise", "rsx-nowrap"]),
        ("dup", vec!["clockmap", "--premise", "spu-nowrap", "--premise", "spu-nowrap"]),
        ("flag", vec!["clockmap", "--nowrap"]),
    ] {
        let (code, _) = cm_cli(&args, Some(&clean), tag);
        assert_eq!(code, Some(3), "{tag}");
    }
    let (code, _) = cm_cli(&["clockmap", "--premise"], None, "novalue");
    assert_eq!(code, Some(3));
    let (code, _) = cm_cli(&["clockmap"], None, "nofile");
    assert_eq!(code, Some(3));
    let path = std::env::temp_dir().join(format!("ps3dk-prof-cm-{}-two.prof", std::process::id()));
    std::fs::write(&path, &clean).unwrap();
    let p = path.to_string_lossy().into_owned();
    let out =
        std::process::Command::new(env!("CARGO_BIN_EXE_ps3dk-prof")).args(["clockmap", &p, &p]).output().unwrap();
    let _ = std::fs::remove_file(&path);
    assert_eq!(out.status.code(), Some(3), "two paths");
    let (code, out) = cm_cli(&["clockmap", "--premise", "spu-nowrap"], Some(&clean), "w32");
    assert_eq!(code, Some(0));
    assert!(out.lines().any(|l| l.contains("SPU lane") && l.contains("premises C,A,S,W32")), "{out}");
}

// ---- conditional durations (P3.5): parsed captures --------------------------------------------------------------

use ps3dk_prof::durations::{
    durations, lines as du_lines, DLimit, DurState, DurationBudgets, DurationMap, Inventory, ZoneDur,
};

/// SPU brackets under W32: dec 1000 -> f = TOP-1000 in [10, 20]; dec 900 in [30, 40]: B = [1/10, 3/10].
fn du_cal() -> Chunk {
    calibration(SPU_D, 1, 0, &[(1000, 10, 20), (900, 30, 40)])
}

/// lane_only with an explicit FINAL open_begins value.
fn lane_fin(body: Vec<Chunk>, fin: (u8, u64, u64, u64, u32)) -> Vec<Chunk> {
    let (state, next, events, lost, open) = fin;
    let mut v = vec![
        domains(&[(SPU_D, 1, LANE)]),
        images(0, b"spu-build", 2),
        lanes(&[(LANE, SPU_D, 0, 1)]),
        epochs(&[(SPU_D, 1, 1, 0)]),
    ];
    v.extend(body);
    v.push(finals(&[(LANE, 1, state, next, events, lost, open)]));
    v
}

fn du_run(name: &str, ext: Vec<Chunk>, b: &DurationBudgets) -> (ExtCapture, CaptureMap, DurationMap) {
    let x = capture(name, ext);
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let d = durations(&x, &c, b);
    (x, c, d)
}

fn du(name: &str, ext: Vec<Chunk>) -> (ExtCapture, CaptureMap, DurationMap) {
    du_run(name, ext, &DurationBudgets::default())
}

fn dstate(d: &DurationMap, epoch: u32) -> &DurState {
    &d.epochs.iter().find(|e| e.source == SourceKey::Lane(LANE) && e.epoch == epoch).expect("a duration entry").state
}

fn summaries(d: &DurationMap, epoch: u32) -> (&Inventory, &Vec<ZoneDur>) {
    match dstate(d, epoch) {
        DurState::Summaries { inventory, zones } => (inventory, zones),
        other => panic!("expected summaries, got {other:?}"),
    }
}

fn zone_of(z: &[ZoneDur], desc: u32) -> &ZoneDur {
    z.iter().find(|z| z.desc == desc).unwrap_or_else(|| panic!("no zone {desc}"))
}

/// "[lo, hi]" with ( for an open lower end and +inf).
fn btext(b: &ps3dk_prof::durations::Bound) -> String {
    format!(
        "{}{}, {}",
        if b.lo_open { "(" } else { "[" },
        b.lo,
        b.hi.as_ref().map_or("+inf)".to_string(), |h| format!("{h}]"))
    )
}

#[test]
fn du_nested_pair_inclusive_and_exclusive_scale_with_one_b() {
    // zone 0 at dec 990..950 (d = 40) holding zone 1 at 980..970 (d = 10): exclusive 30.  B = [1/10, 3/10].
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 1, 1), (970, 1, 2, 1), (950, 0, 2, 0)]),
    ];
    let (_, _, d) = du("du-nested", lane_only(body, (1, 4, 4, 0)));
    let (inv, z) = summaries(&d, 1);
    assert_eq!((inv.events, inv.paired, inv.pairs, inv.marks, inv.open, inv.unclassified), (4, 4, 2, 0, Some(0), 0));
    let p = zone_of(z, 0);
    let i = p.inclusive.as_ref().unwrap();
    assert_eq!((i.n, i.sum, btext(&i.b_sum)), (1, 40, "[4, 12]".to_string()));
    let e = p.exclusive.as_ref().unwrap();
    assert_eq!((e.sum, btext(&e.b_sum)), (30, "[3, 9]".to_string()));
    let c = zone_of(z, 1);
    assert_eq!(btext(&c.inclusive.as_ref().unwrap().b_sum), "[1, 3]");
    assert_eq!(c.exclusive.as_ref().unwrap().sum, 10, "a leaf's exclusive is its inclusive");
}

#[test]
fn du_exclusive_is_joint_not_independent_interval_subtraction() {
    // B = [1, 2]: brackets dec 1000 in [100, 101], dec 990 in [111, 120] (df = 10: b in [(111-101)/10, (120-100)/10]).
    // Parent d = 10 (dec 1000..990) holding child d = 4 (997..993): exclusive 6 -> [6, 12], never [2, 16].
    let body = vec![
        calibration(SPU_D, 1, 0, &[(1000, 100, 101), (990, 111, 120)]),
        spu_events(LANE, 1, 0, &[(1000, 0, 1, 0), (997, 1, 1, 1), (993, 1, 2, 1), (990, 0, 2, 0)]),
    ];
    let (_, c, d) = du("du-joint", lane_only(body, (1, 4, 4, 0)));
    let EpochState::Compatible(s) = state(&c, SourceKey::Lane(LANE), 1) else { panic!() };
    assert_eq!((s.slope_lo.to_string(), s.slope_hi.as_ref().unwrap().to_string()), ("1".into(), "2".into()));
    let (_, z) = summaries(&d, 1);
    assert_eq!(btext(&zone_of(z, 0).exclusive.as_ref().unwrap().b_sum), "[6, 12]");
}

#[test]
fn du_child_outside_parent_contradicts_order() {
    // f (= TOP - dec, relative): parent 0..100, child 110..120 -> sequence order f 0, 110, 120, 100: one decrease.
    let t = 2_000u32;
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(t, 0, 1, 0), (t - 110, 1, 1, 1), (t - 120, 1, 2, 1), (t - 100, 0, 2, 0)]),
    ];
    let (_, c, d) = du("du-outside", lane_only(body, (1, 4, 4, 0)));
    assert!(matches!(state(&c, SourceKey::Lane(LANE), 1), EpochState::Compatible(_)), "clockmap unchanged");
    let DurState::Contradicted { decreases, first, zones, inventory } = dstate(&d, 1) else { panic!("{d:?}") };
    let top = SPU_DEC_TOP;
    assert_eq!(*decreases, 1);
    assert_eq!(*first, (2, 3, top - i128::from(t - 120), top - i128::from(t - 100)));
    assert_eq!(inventory.pairs, 2);
    let got: Vec<(u32, i128)> = zones.iter().map(|z| (z.desc, z.sum)).collect();
    assert_eq!(got, vec![(0, 100), (1, 10)]);
}

#[test]
fn du_overlapping_siblings_contradict_order() {
    // parent 0..100; children 20..80 then 30..90 (sequence order f: 0, 20, 80, 30, 90, 100): 80 -> 30 decreases.
    let t = 2_000u32;
    let ev = [(t, 0, 1, 0), (t - 20, 1, 1, 1), (t - 80, 1, 2, 1), (t - 30, 1, 1, 1), (t - 90, 1, 2, 1), (t - 100, 0, 2, 0)];
    let (_, _, d) = du("du-siblings", lane_only(vec![du_cal(), spu_events(LANE, 1, 0, &ev)], (1, 6, 6, 0)));
    let DurState::Contradicted { decreases, first, .. } = dstate(&d, 1) else { panic!("{d:?}") };
    assert_eq!((*decreases, first.0, first.1), (1, 2, 3));
}

#[test]
fn du_a_negative_pair_reports_signed_raw_deltas_only() {
    // BEGIN dec 900, END dec 950: d = -50 in the premise coordinate.
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(900, 0, 1, 0), (950, 0, 2, 0)])];
    let (x, c, d) = du("du-negative", lane_only(body, (1, 2, 2, 0)));
    let DurState::Contradicted { zones, .. } = dstate(&d, 1) else { panic!("{d:?}") };
    assert_eq!((zones[0].sum, zones[0].min, zones[0].max), (-50, -50, -50));
    let cap = match load(&ext_file(lane_only(vec![du_cal(), spu_events(LANE, 1, 0, &[(900, 0, 1, 0), (950, 0, 2, 0)])], (1, 2, 2, 0))).bytes()) {
        Outcome::Analysed(a) => a.capture,
        Outcome::Malformed(m) => panic!("{m}"),
    };
    let text = du_lines(&cap, &x, &c, &d).join("\n");
    assert!(text.contains("ORDER CONTRADICTED (1 decrease(s)"), "{text}");
    assert!(text.contains("signed raw counter deltas sum -50 min -50 max -50 (no time claimed)"), "{text}");
    assert!(!text.contains("inclusive ("), "no time summary: {text}");
}

#[test]
fn du_a_zero_pair_is_zero_and_no_pairs_is_unavailable() {
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(950, 0, 1, 0), (950, 0, 2, 0)])];
    let (_, _, d) = du("du-zero", lane_only(body, (1, 2, 2, 0)));
    let (_, z) = summaries(&d, 1);
    let i = zone_of(z, 0).inclusive.as_ref().unwrap();
    assert_eq!((i.sum, btext(&i.b_sum)), (0, "[0, 0]".to_string()));
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(950, 0, M, 0), (940, 0, M, 0)])];
    let (_, _, d) = du("du-marks", lane_only(body, (1, 2, 2, 0)));
    let (inv, z) = summaries(&d, 1);
    assert_eq!((inv.marks, inv.pairs, z.len()), (2, 0, 0), "no eligible pairs: no zone, never a zero");
}

#[test]
fn du_protocol_error_events_are_unclassified_not_repaired() {
    // The t4 shape: seq 0 BEGIN, seq 1 END of another descriptor (protocol error), seq 2 MARK: all unclassified.
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 2, 0)]), spu_events(LANE, 1, 2, &[(970, 0, M, 0)])];
    let (_, _, d) = du("du-protocol", lane_only(body, (1, 3, 3, 0)));
    let (inv, z) = summaries(&d, 1);
    assert_eq!((inv.events, inv.paired, inv.marks, inv.open, inv.unclassified), (3, 0, 0, None, 3));
    assert!(z.is_empty());
    // An accepted pair, then an open BEGIN, then the error: the pair stays eligible; the rest is unclassified.
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0), (970, 0, 1, 0), (960, 1, 2, 0)])];
    let (_, _, d) = du("du-pair-error", lane_only(body, (1, 4, 4, 0)));
    let (inv, z) = summaries(&d, 1);
    assert_eq!((inv.paired, inv.pairs, inv.open, inv.unclassified), (2, 1, None, 2));
    assert_eq!(zone_of(z, 0).inclusive.as_ref().unwrap().sum, 10);
}

#[test]
fn du_loss_then_resync_never_spans_the_loss() {
    // BEGIN (seq 0), LOSS of seq 1, then a depth-0 resync pair (seq 2, 3): the first BEGIN is censored.
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0)]),
        loss(LANE, 1, 1, 1),
        spu_events(LANE, 1, 2, &[(970, 0, 1, 0), (960, 0, 2, 0)]),
    ];
    let (x, _, d) = du("du-loss", lane_only(body, (1, 4, 3, 1)));
    let (inv, z) = summaries(&d, 1);
    assert_eq!((inv.events, inv.paired, inv.censored), (3, 2, 1));
    assert_eq!(inv.loss_records_in_span, Some(1));
    let s = x.sources.iter().find(|s| s.key == SourceKey::Lane(LANE)).unwrap();
    let cv = counter_variation(&x, s).unwrap();
    assert_eq!(cv[0].loss_records_in_event_span, inv.loss_records_in_span, "the counter_variation fact, exactly");
    assert_eq!(zone_of(z, 0).pairs, 1, "only the resync pair; nothing crosses the LOSS");
}

#[test]
fn du_two_epochs_are_isolated_with_their_own_inventory() {
    // Epoch 1: a pair, then a BEGIN left open by the transition (censored).  Epoch 2: one pair, own brackets.
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0), (970, 0, 1, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        calibration(SPU_D, 2, 0, &[(0xFFFF_1000, 500, 510), (0xFFFE_0000, 600, 700)]),
        spu_events(LANE, 2, 3, &[(0xFFFF_0000, 0, 1, 0), (0xFFFE_8000, 0, 2, 0)]),
    ];
    let (_, _, d) = du("du-two-epochs", lane_only(body, (1, 5, 5, 0)));
    let (i1, z1) = summaries(&d, 1);
    assert_eq!((i1.events, i1.paired, i1.censored), (3, 2, 1));
    assert_eq!(zone_of(z1, 0).inclusive.as_ref().unwrap().sum, 10);
    let (i2, z2) = summaries(&d, 2);
    assert_eq!((i2.events, i2.paired, i2.censored), (2, 2, 0));
    assert_eq!(zone_of(z2, 0).inclusive.as_ref().unwrap().sum, 0x8000, "the same descriptor in another epoch, alone");
}

#[test]
fn du_open_parent_with_a_completed_child() {
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 1, 1), (970, 1, 2, 1)])];
    let (_, _, d) = du("du-open-parent", lane_fin(body, (1, 3, 3, 0, 1)));
    let (inv, z) = summaries(&d, 1);
    assert_eq!((inv.paired, inv.open), (2, Some(1)));
    let c = zone_of(z, 1);
    assert_eq!((c.inclusive.as_ref().unwrap().sum, c.exclusive.as_ref().unwrap().sum), (10, 10));
    assert!(z.iter().all(|z| z.desc != 0), "the open parent has no duration");
}

#[test]
fn du_percentiles_need_twenty_eligible_pairs() {
    // Pairs of zone 0 with d = 1, 2, ..., n (back to back, nondecreasing f).
    let mk = |n: u32| {
        let mut ev = Vec::new();
        let mut dec = 999u32;
        for i in 1..=n {
            ev.push((dec, 0u16, 1u8, 0u8));
            dec -= i;
            ev.push((dec, 0, 2, 0));
        }
        lane_only(vec![du_cal(), spu_events(LANE, 1, 0, &ev)], (1, 2 * u64::from(n), 2 * u64::from(n), 0))
    };
    let (_, _, d) = du("du-p19", mk(19));
    let i = zone_of(summaries(&d, 1).1, 0).inclusive.clone().unwrap();
    assert_eq!((i.n, i.p50, i.p90), (19, None, None));
    let (_, _, d) = du("du-p20", mk(20));
    let i = zone_of(summaries(&d, 1).1, 0).inclusive.clone().unwrap();
    assert_eq!((i.n, i.p50, i.p90, i.sum), (20, Some(10), Some(18), 210));
}

#[test]
fn du_rsx_pair_uses_its_domains_own_slope() {
    let (x, c, d) = {
        let x = capture("du-rsx", cm_ext());
        let c = map_capture(&x, Premises::default(), &Budgets::default());
        let d = durations(&x, &c, &DurationBudgets::default());
        (x, c, d)
    };
    assert!(d.epochs.iter().all(|e| e.source == SourceKey::Gpu(RSX_D)), "default premises: SPU stays AMBIGUOUS-LIFT");
    let DurState::Summaries { zones, .. } = &d.epochs[0].state else { panic!("{d:?}") };
    // GPU pair 5000..6000: d = 1000, B = [29/300, 31/300] -> [290/3, 310/3].
    assert_eq!(btext(&zones[0].inclusive.as_ref().unwrap().b_sum), "[290/3, 310/3]");
    let cap = match load(&ext_file(cm_ext()).bytes()) {
        Outcome::Analysed(a) => a.capture,
        Outcome::Malformed(m) => panic!("{m}"),
    };
    let text = du_lines(&cap, &x, &c, &d);
    assert!(text.iter().any(|l| l.contains("desc 3 gpuzone")), "{text:#?}");
}

#[test]
fn du_clockmap_lines_and_map_are_unchanged_by_durations() {
    let x = capture("du-identity", cm_ext());
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let before = c.clone();
    let d = durations(&x, &c, &DurationBudgets::default());
    assert_eq!(c, before);
    let cap = match load(&ext_file(cm_ext()).bytes()) {
        Outcome::Analysed(a) => a.capture,
        Outcome::Malformed(m) => panic!("{m}"),
    };
    let plain = clockmap_lines(&x, &c);
    let with = du_lines(&cap, &x, &c, &d);
    // Every clockmap line appears, in order, unchanged; the rest are duration lines.
    let mut it = with.iter();
    for l in &plain {
        assert!(it.any(|w| w == l), "missing or reordered clockmap line {l:?}");
    }
    assert!(with.iter().filter(|w| !plain.contains(w)).all(|w| w.starts_with("    durations: ") || w.starts_with("      ")));
}

#[test]
fn du_budget_exact_one_over_and_scan_boundary() {
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 1, 1), (970, 1, 2, 1), (950, 0, 2, 0)]),
    ];
    let ext = lane_only(body, (1, 4, 4, 0));
    let full = du("du-budget", ext.clone()).2;
    let need = full.used;
    let with = |ops: u64| du_run("du-budget", ext.clone(), &DurationBudgets { capture_ops: ops, ..DurationBudgets::default() }).2;
    let exact = with(need);
    assert_eq!((exact.epochs.clone(), exact.exhausted_at), (full.epochs.clone(), None));
    let short = with(need - 1);
    assert_eq!(dstate(&short, 1), &DurState::Unsupported(DLimit::CaptureOps));
    assert_eq!(short.exhausted_at, Some((SourceKey::Lane(LANE), 1)));
    // The preliminary source scan unaffordable: nothing partial, the epoch unsupported.
    let none = with(1);
    assert_eq!(dstate(&none, 1), &DurState::Unsupported(DLimit::CaptureOps));
}

#[test]
fn du_epoch_local_caps_isolate_their_epoch() {
    // Epoch 1: 6 MARKs (over a 4-event cap); epoch 2: one pair.  Only epoch 1 is unsupported.
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (989, 0, M, 0), (988, 0, M, 0), (987, 0, M, 0), (986, 0, M, 0), (985, 0, M, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        calibration(SPU_D, 2, 0, &[(0xFFFF_1000, 500, 510), (0xFFFE_0000, 600, 700)]),
        spu_events(LANE, 2, 6, &[(0xFFFF_0000, 0, 1, 0), (0xFFFE_8000, 0, 2, 0)]),
    ];
    let ext = lane_only(body, (1, 8, 8, 0));
    let b = DurationBudgets { max_events_per_epoch: 4, ..DurationBudgets::default() };
    let d = du_run("du-cap-events", ext.clone(), &b).2;
    assert_eq!(dstate(&d, 1), &DurState::Unsupported(DLimit::Events));
    assert!(matches!(dstate(&d, 2), DurState::Summaries { .. }), "{d:?}");
    assert_eq!(d.exhausted_at, None);
    // Exactly at the cap: summarised.
    let at = du_run("du-cap-events", ext, &DurationBudgets { max_events_per_epoch: 6, ..DurationBudgets::default() }).2;
    assert!(matches!(dstate(&at, 1), DurState::Summaries { .. }));
    // Pairs and zones caps.
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 1, 1), (970, 1, 2, 1), (950, 0, 2, 0)]),
    ];
    let ext = lane_only(body, (1, 4, 4, 0));
    let p = du_run("du-cap-pairs", ext.clone(), &DurationBudgets { max_pairs_per_epoch: 1, ..DurationBudgets::default() }).2;
    assert_eq!(dstate(&p, 1), &DurState::Unsupported(DLimit::Pairs));
    let z = du_run("du-cap-zones", ext.clone(), &DurationBudgets { max_zones_per_epoch: 1, ..DurationBudgets::default() }).2;
    assert_eq!(dstate(&z, 1), &DurState::Unsupported(DLimit::Zones));
    let ok = du_run("du-cap-zones", ext, &DurationBudgets { max_pairs_per_epoch: 2, max_zones_per_epoch: 2, ..DurationBudgets::default() }).2;
    assert!(matches!(dstate(&ok, 1), DurState::Summaries { .. }));
}

#[test]
fn du_defensive_incomplete_child_propagates_to_every_ancestor() {
    // DEFENSIVE (synthetic, not reachable from the reader): the nested fixture with the child pair removed from the
    // reader's pairs and its events re-listed as censored/unpaired.  The parent's inside then holds non-member events,
    // so its exclusive is unavailable (never inflated); its inclusive is unchanged.
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 1, 1), (970, 1, 2, 1), (950, 0, 2, 0)]),
    ];
    let mut x = capture("du-defensive", lane_only(body, (1, 4, 4, 0)));
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let s = x.sources.iter_mut().find(|s| s.key == SourceKey::Lane(LANE)).unwrap();
    s.raw.pairs.retain(|p| p.desc == 0);
    s.raw.censored.push(1);
    s.raw.unpaired.push(2);
    let d = durations(&x, &c, &DurationBudgets::default());
    let (_, z) = summaries(&d, 1);
    let p = zone_of(z, 0);
    assert_eq!((p.inclusive.as_ref().unwrap().sum, p.exclusive.is_none(), p.exclusive_unavailable), (40, true, 1));
    // A remainder on a source WITHOUT a protocol error is an inventory defect, not 'unclassified'.
    let mut x2 = x.clone();
    let s2 = x2.sources.iter_mut().find(|s| s.key == SourceKey::Lane(LANE)).unwrap();
    s2.raw.unpaired.clear();
    let d2 = durations(&x2, &c, &DurationBudgets::default());
    assert_eq!(dstate(&d2, 1), &DurState::Unsupported(DLimit::Inventory));
    // A double claim is an inventory defect too.
    let mut x3 = x.clone();
    x3.sources.iter_mut().find(|s| s.key == SourceKey::Lane(LANE)).unwrap().raw.marks.push(0);
    assert_eq!(dstate(&durations(&x3, &c, &DurationBudgets::default()), 1), &DurState::Unsupported(DLimit::Inventory));
}

/// The clockmap + duration text of a parsed capture (W32 selected).
fn du_text(name: &str, ext: Vec<Chunk>) -> Vec<String> {
    let bytes = ext_file(ext).bytes();
    dump(name, &bytes);
    let cap = match load(&bytes) {
        Outcome::Analysed(a) => a.capture,
        Outcome::Malformed(m) => panic!("{name}: {m}"),
    };
    let x = cap.ext.clone().expect("extension capture");
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let d = durations(&x, &c, &DurationBudgets::default());
    du_lines(&cap, &x, &c, &d)
}

#[test]
fn du_scratch_is_bounded_by_the_epoch_not_the_source() {
    // An over-cap epoch (6 MARKs, cap 4) followed by a small eligible epoch (one pair): only the small epoch's
    // scratch is ever allocated (class 2 + pairs 1 + coordinates 2 + positions 2 = 7 elements).
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, M, 0), (989, 0, M, 0), (988, 0, M, 0), (987, 0, M, 0), (986, 0, M, 0), (985, 0, M, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        calibration(SPU_D, 2, 0, &[(0xFFFF_1000, 500, 510), (0xFFFE_0000, 600, 700)]),
        spu_events(LANE, 2, 6, &[(0xFFFF_0000, 0, 1, 0), (0xFFFE_8000, 0, 2, 0)]),
    ];
    let d = du_run("du-hw-cap", lane_only(body, (1, 8, 8, 0)), &DurationBudgets { max_events_per_epoch: 4, ..DurationBudgets::default() }).2;
    assert_eq!(dstate(&d, 1), &DurState::Unsupported(DLimit::Events));
    assert_eq!(d.scratch_high_water, 7, "only epoch 2's scratch");
    // A large INELIGIBLE epoch (no brackets: NO-OBSERVATIONS, 200 MARKs) beside a small eligible one: same bound.
    let marks: Vec<(u32, u16, u8, u8)> = (0..200u32).map(|i| (5_000 - i, 0, M, 0)).collect();
    let body = vec![
        spu_events(LANE, 1, 0, &marks),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        calibration(SPU_D, 2, 0, &[(0xFFFF_1000, 500, 510), (0xFFFE_0000, 600, 700)]),
        spu_events(LANE, 2, 200, &[(0xFFFF_0000, 0, 1, 0), (0xFFFE_8000, 0, 2, 0)]),
    ];
    let (_, c, d) = du("du-hw-ineligible", lane_only(body, (1, 202, 202, 0)));
    assert_eq!(state(&c, SourceKey::Lane(LANE), 1), &EpochState::NoObservations);
    assert!(d.epochs.iter().all(|e| e.epoch == 2), "only the eligible epoch has a duration entry");
    assert!(matches!(dstate(&d, 2), DurState::Summaries { .. }));
    assert_eq!(d.scratch_high_water, 7, "the 200-event epoch allocated nothing");
}

#[test]
fn du_inventory_names_a_protocol_error_only_when_the_source_has_one() {
    // A clean source: no unclassified term, no error qualifier.
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 1, 1), (970, 1, 2, 1), (950, 0, 2, 0)]),
    ];
    let t = du_text("du-text-clean", lane_only(body, (1, 4, 4, 0)));
    let clean = "    durations: premises C,A,S,E,W32 | inventory: 4 events = 4 paired (2 pairs) + 0 marks + 0 censored + 0 \
                 unpaired + open 0 | LOSS records inside the observed event-sequence span: 0";
    assert!(t.iter().any(|l| l == clean), "{t:#?}");
    assert!(!t.iter().any(|l| l.contains("PROTOCOL ERROR")), "{t:#?}");
    // A real terminal error (the t4 shape, its earlier BEGIN included): open unknown, 3 unclassified, qualified.
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 2, 0)]), spu_events(LANE, 1, 2, &[(970, 0, M, 0)])];
    let t = du_text("du-text-error", lane_only(body, (1, 3, 3, 0)));
    let err = "    durations: premises C,A,S,E,W32 | inventory: 3 events = 0 paired (0 pairs) + 0 marks + 0 censored + 0 \
               unpaired + open unknown + 3 unclassified (no pairing claim; source PROTOCOL ERROR) | LOSS records inside the \
               observed event-sequence span: 0";
    assert!(t.iter().any(|l| l == err), "{t:#?}");
    // A terminal error with ZERO remainder is not constructible from the reader: the offending event itself is
    // stored and never placed in a list, so an error source always has at least one unclassified event.
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0), (970, 1, 2, 0)])];
    let (_, _, d) = du("du-text-error-min", lane_only(body, (1, 3, 3, 0)));
    let (inv, _) = summaries(&d, 1);
    assert!(inv.protocol_error && inv.unclassified >= 1 && inv.open.is_none(), "{inv:?}");
}

#[test]
fn du_defensive_incomplete_mark_reaches_the_grandparent() {
    // DEFENSIVE (synthetic): grandparent (desc 0, depth 0) > parent (desc 1, depth 1) > child (desc 0, depth 2); the
    // child pair is removed and its events re-listed as censored/unpaired.  The non-member events sit directly inside
    // the parent, and the incomplete mark must reach the grandparent as the pairs close.
    let ev = [(990, 0, 1, 0), (985, 1, 1, 1), (980, 0, 1, 2), (975, 0, 2, 2), (970, 1, 2, 1), (950, 0, 2, 0)];
    let mut x = capture("du-defensive-3", lane_only(vec![du_cal(), spu_events(LANE, 1, 0, &ev)], (1, 6, 6, 0)));
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let s = x.sources.iter_mut().find(|s| s.key == SourceKey::Lane(LANE)).unwrap();
    s.raw.pairs.retain(|p| p.depth < 2);
    s.raw.censored.push(2);
    s.raw.unpaired.push(3);
    let d = durations(&x, &c, &DurationBudgets::default());
    let (_, z) = summaries(&d, 1);
    let (g, p) = (zone_of(z, 0), zone_of(z, 1));
    assert_eq!((g.exclusive.is_none(), g.exclusive_unavailable), (true, 1), "grandparent: {g:?}");
    assert_eq!((p.exclusive.is_none(), p.exclusive_unavailable), (true, 1), "parent: {p:?}");
}

// ---- F2a: SPU/RSX reader pairs placed on PPU frames (t_d9789c06) -------------------------------------------------

use ps3dk_prof::clockmap::{map_capture_models, ModelSink};
use ps3dk_prof::durations::{durations_pairs, PairSink};
use ps3dk_prof::frames_foreign::{
    foreign_frames, EpochF2, FLimit, ForeignFrames, FrameBudgets, LaneF2, PlacementState, Refusal, Retention,
};

/// The v1 scenario with a third PPU FRAME: measured frames [0, 150) and [150, 300) on owner 1.  Under cm_ext the SPU
/// pair (W32) begins at t in [10, 40] (frame 0) and the RSX pair at t(5000), about [200, 210] (frame 1).
fn f2_file(ext: Vec<Chunk>) -> File {
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
    f
}

fn analysed(name: &str, f: File) -> ps3dk_prof::Analysis {
    let bytes = f.bytes();
    dump(name, &bytes);
    match load(&bytes) {
        Outcome::Analysed(a) => *a,
        Outcome::Malformed(m) => panic!("{name}: {m}"),
    }
}

fn f2(a: &ps3dk_prof::Analysis, p: Premises, b: &FrameBudgets, keep: &Retention) -> Result<ForeignFrames, Refusal> {
    foreign_frames(a, 1, p, &Budgets::default(), &DurationBudgets::default(), b, keep)
}

fn f2_lane(r: &ForeignFrames, key: SourceKey) -> &LaneF2 {
    r.lanes.iter().find(|l| l.source == key).unwrap_or_else(|| panic!("no lane {key}: {r:?}"))
}

#[test]
fn f2_places_spu_and_rsx_pairs_on_their_own_frames() {
    let a = analysed("f2-place", f2_file(cm_ext()));
    let r = f2(&a, NOWRAP, &FrameBudgets::default(), &Retention::default()).unwrap();
    assert_eq!(r.state, PlacementState::Placed);
    assert_eq!(r.frames.measured.len(), 2);
    let spu = f2_lane(&r, SourceKey::Lane(LANE));
    assert_eq!(spu.epochs[0].state, EpochF2::Placed);
    assert_eq!(spu.frames.len(), 1, "{spu:?}");
    let c = &spu.frames[0];
    assert_eq!((c.frame, c.included, c.bar_epochs), (0, 1, 1));
    // d = 10 under W32, B = [1/10, 3/10]
    assert_eq!(btext(c.bar.as_ref().unwrap()), "[1, 3]");
    let rsx = f2_lane(&r, SourceKey::Gpu(RSX_D));
    let c = &rsx.frames[0];
    assert_eq!((c.frame, c.included), (1, 1), "{rsx:?}");
    // d = 1000, B = [29/300, 31/300]: the same bound durations reports for this pair
    assert_eq!(btext(c.bar.as_ref().unwrap()), "[290/3, 310/3]");
    assert_eq!(btext(&c.zones[&(None, GPU_DESC)].bound), "[290/3, 310/3]");
    // lanes are separate: no lane holds the other lane's frame
    assert!(spu.frames.iter().all(|c| c.frame == 0) && rsx.frames.iter().all(|c| c.frame == 1));
}

#[test]
fn f2_default_premises_leave_spu_ambiguous_lift_with_its_pair_count() {
    let a = analysed("f2-default", f2_file(cm_ext()));
    let r = f2(&a, Premises::default(), &FrameBudgets::default(), &Retention::default()).unwrap();
    let spu = f2_lane(&r, SourceKey::Lane(LANE));
    assert_eq!((spu.epochs[0].state.clone(), spu.epochs[0].pairs), (EpochF2::AmbiguousLift, Some(1)));
    assert!(spu.frames.is_empty(), "never drawn: {spu:?}");
    assert_eq!(f2_lane(&r, SourceKey::Gpu(RSX_D)).epochs[0].state, EpochF2::Placed);
}

#[test]
fn f2_owner_refusals() {
    let a = analysed("f2-owner", f2_file(cm_ext()));
    let r = foreign_frames(
        &a,
        99,
        NOWRAP,
        &Budgets::default(),
        &DurationBudgets::default(),
        &FrameBudgets::default(),
        &Retention::default(),
    );
    assert_eq!(r, Err(Refusal::NotAnOwner));
    let few = FrameBudgets { max_frames: 2, ..FrameBudgets::default() };
    assert_eq!(f2(&a, NOWRAP, &few, &Retention::default()), Err(Refusal::TooManyFrames));
}

/// The scoped-reuse entry points return exactly the old results, at every budget boundary tried.
#[test]
fn f2_retaining_entry_points_equal_the_old_ones() {
    for (name, ext) in [("f2-eq-std", std_ext()), ("f2-eq-cm", cm_ext())] {
        let x = capture(name, ext);
        for p in [Premises::default(), NOWRAP] {
            let full = map_capture(&x, p, &Budgets::default()).used;
            for capture in [0, 1, full / 2, full.saturating_sub(1), full, full + 1, Budgets::default().capture] {
                let b = Budgets { capture, ..Budgets::default() };
                let plain = map_capture(&x, p, &b);
                let mut ms = ModelSink::new(u64::MAX);
                assert_eq!(map_capture_models(&x, p, &b, &mut ms), plain, "{name} capture {capture}");
                let mut none = ModelSink::new(0);
                assert_eq!(map_capture_models(&x, p, &b, &mut none), plain, "{name} refused sink, capture {capture}");
            }
            let c = map_capture(&x, p, &Budgets::default());
            let full = durations(&x, &c, &DurationBudgets::default()).used;
            for capture_ops in [0, 1, full / 2, full.saturating_sub(1), full, full + 1] {
                let b = DurationBudgets { capture_ops, ..DurationBudgets::default() };
                let plain = durations(&x, &c, &b);
                let mut ps = PairSink::new(u64::MAX);
                assert_eq!(durations_pairs(&x, &c, &b, &mut ps), plain, "{name} capture_ops {capture_ops}");
                let mut none = PairSink::new(0);
                assert_eq!(durations_pairs(&x, &c, &b, &mut none), plain, "{name} refused sink, ops {capture_ops}");
            }
        }
    }
}

#[test]
fn f2_retention_exhaustion_refuses_f2_only() {
    let a = analysed("f2-keep", f2_file(cm_ext()));
    let x = a.capture.ext.as_ref().unwrap();
    let mut ms = ModelSink::new(0);
    let c = map_capture_models(x, NOWRAP, &Budgets::default(), &mut ms);
    assert!(ms.refused && ms.models.is_empty());
    assert_eq!(c, map_capture(x, NOWRAP, &Budgets::default()));
    let tiny = Retention { model_vertices: 0, ..Retention::default() };
    assert_eq!(f2(&a, NOWRAP, &FrameBudgets::default(), &tiny), Err(Refusal::RetentionModels));
    let tiny = Retention { pair_facts: 1, ..Retention::default() }; // two epochs of one pair each
    assert_eq!(f2(&a, NOWRAP, &FrameBudgets::default(), &tiny), Err(Refusal::RetentionPairs));
    let exact = Retention { pair_facts: 2, ..Retention::default() };
    assert!(f2(&a, NOWRAP, &FrameBudgets::default(), &exact).is_ok());
}

type F2States = (EpochF2, EpochF2, Option<(SourceKey, u32)>);

/// capture_ops: below what both epochs need, the later epoch is refused once the earlier fits, the earlier is never
/// refused while the later is placed, and the refusal set is the same on every run.
#[test]
fn f2_capture_ops_refuses_the_current_and_later_epochs() {
    let a = analysed("f2-cap", f2_file(cm_ext()));
    let full = f2(&a, NOWRAP, &FrameBudgets::default(), &Retention::default()).unwrap();
    assert!(full.exhausted_at.is_none());
    let order: Vec<SourceKey> = full.lanes.iter().map(|l| l.source).collect();
    assert_eq!(order.len(), 2);
    let states = |cap: u64| -> Option<F2States> {
        let b = FrameBudgets { capture_ops: cap, ..FrameBudgets::default() };
        let r = f2(&a, NOWRAP, &b, &Retention::default()).ok()?;
        Some((f2_lane(&r, order[0]).epochs[0].state.clone(), f2_lane(&r, order[1]).epochs[0].state.clone(), r.exhausted_at))
    };
    let mut split = None;
    for cap in 0..=full.used {
        let s = states(cap);
        assert_eq!(s, states(cap), "reproducible at {cap}");
        match s {
            None => assert!(split.is_none(), "an owner-scan refusal can only come first ({cap})"),
            Some((first, second, at)) => {
                if first == EpochF2::Placed && second == EpochF2::Unsupported(FLimit::CaptureOps) {
                    assert_eq!(at, Some((order[1], 1)));
                    split.get_or_insert(cap);
                }
                if first == EpochF2::Unsupported(FLimit::CaptureOps) {
                    assert_eq!(second, EpochF2::Unsupported(FLimit::CaptureOps), "later epochs follow at {cap}");
                    assert_eq!(at, Some((order[0], 1)), "exhausted_at names the FIRST refused epoch at {cap}");
                    assert!(split.is_none());
                }
            }
        }
    }
    assert!(split.is_some(), "some budget places the first epoch and refuses the second");
    let (f, s, at) = states(full.used).unwrap();
    assert_eq!((f, s, at), (EpochF2::Placed, EpochF2::Placed, None), "exactly the full use suffices");
    let (_, s, _) = states(full.used - 1).unwrap();
    assert_eq!(s, EpochF2::Unsupported(FLimit::CaptureOps), "one under refuses the last epoch");
}

fn f2_nested() -> Vec<Chunk> {
    // du_nested_pair_inclusive_and_exclusive_scale_with_one_b: zone 0 (d 40) holding zone 1 (d 10), B = [1/10, 3/10]
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 1, 1, 1), (970, 1, 2, 1), (950, 0, 2, 0)]),
    ];
    lane_only(body, (1, 4, 4, 0))
}

#[test]
fn f2_pair_facts_of_a_parsed_nested_capture() {
    let x = capture("f2-facts-nested", f2_nested());
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let mut ps = PairSink::new(u64::MAX);
    durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let facts = &ps.epochs[&(SourceKey::Lane(LANE), 1)];
    let mut got: Vec<_> =
        facts.iter().map(|f| (f.desc, f.image, f.fe - f.fb, f.inclusive, f.exclusive, f.top_level, f.extrapolated)).collect();
    got.sort();
    assert_eq!(got, vec![(0, Some(0), 40, 40, Some(30), true, false), (1, Some(0), 10, 10, Some(10), false, false)]);
    assert_eq!(facts.iter().find(|f| f.desc == 0).unwrap().fb, SPU_DEC_TOP - 990);
}

#[test]
fn f2_bar_is_top_level_inclusive_and_zone_rows_are_exclusive() {
    let a = analysed("f2-nested", f2_file(f2_nested()));
    let r = f2(&a, NOWRAP, &FrameBudgets::default(), &Retention::default()).unwrap();
    let spu = f2_lane(&r, SourceKey::Lane(LANE));
    assert_eq!(spu.frames.len(), 1, "{spu:?}");
    let c = &spu.frames[0];
    assert_eq!((c.frame, c.included), (0, 2));
    assert_eq!(btext(c.bar.as_ref().unwrap()), "[4, 12]", "the parent's inclusive only");
    let rows: Vec<(u32, String)> = c.zones.iter().map(|(k, z)| (k.1, btext(&z.bound))).collect();
    assert_eq!(rows, vec![(0, "[3, 9]".to_string()), (1, "[1, 3]".to_string())]);
}

#[test]
fn f2_extrapolated_end_on_a_parsed_capture_is_excluded_and_counted() {
    // B from dec 1000 / 900 (f = TOP-1000 .. TOP-900); the pair begins at dec 950 (inside) and ends at dec 850 (outside)
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(950, 0, 1, 0), (850, 0, 2, 0)])];
    let ext = lane_only(body, (1, 2, 2, 0));
    let x = capture("f2-extrap-end-facts", ext.clone());
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let mut ps = PairSink::new(u64::MAX);
    durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let f = &ps.epochs[&(SourceKey::Lane(LANE), 1)][0];
    assert!(f.fb >= SPU_DEC_TOP - 1000 && f.fb <= SPU_DEC_TOP - 900, "begin inside: {f:?}");
    assert!(f.extrapolated, "end outside the calibration range: {f:?}");
    let a = analysed("f2-extrap-end", f2_file(ext));
    let r = f2(&a, NOWRAP, &FrameBudgets::default(), &Retention::default()).unwrap();
    let spu = f2_lane(&r, SourceKey::Lane(LANE));
    assert_eq!((spu.counts.extrapolated.definite, spu.counts.extrapolated.total()), (1, 1), "{spu:?}");
    let c = &spu.frames[0];
    assert_eq!((c.included, c.bar.is_none(), c.excluded_extrapolated, c.zones.len()), (0, true, 1, 0), "{c:?}");
}

/// One PPU owner with `n` measured frames (tb 0, 10, ...) and an unrelated owner with `m` frames.
fn f2_two_owners(n: u64, m: u64) -> ps3dk_prof::Analysis {
    let frames = |k: u64| (0..=k).map(|i| frame(0, 10 * i, i)).collect::<Vec<_>>();
    let s = scenario(vec![lane(1, seqd(0, frames(n))), lane(2, seqd(0, frames(m)))]);
    analysed(&format!("f2-owners-{n}-{m}"), s.file())
}

/// owner_frames is one pass over the analysis' frame facts plus one lookup per owner FRAME, all charged before the
/// work: an unrelated owner's frames cost exactly one scan step each, the exact charge suffices, one less refuses.
#[test]
fn f2_owner_frames_work_accounting() {
    use ps3dk_prof::clockmap::{Limit, Meter};
    use ps3dk_prof::frames_foreign::owner_frames;
    let used = |a: &ps3dk_prof::Analysis| {
        let mut m = Meter::unlimited();
        let fs = owner_frames(a, 1, &mut m).unwrap();
        (m.used(), fs.measured.len())
    };
    let small = f2_two_owners(200, 0);
    let big = f2_two_owners(200, 2000);
    let (u0, n0) = used(&small);
    let (u1, n1) = used(&big);
    assert_eq!((n0, n1), (200, 200));
    let extra = big.frames.len() as u64 - small.frames.len() as u64;
    assert!(extra >= 2000, "the unrelated owner has its own frame facts: {extra}");
    assert_eq!(u1 - u0, extra, "an unrelated owner's frame fact costs exactly one scan step");
    // linear, not quadratic, in the owner's frames: the per-frame charge stays logarithmic
    assert!(u0 <= 201 * (4 + 2 * 8) + 3 * 200 + 201, "used {u0}");
    assert!(owner_frames(&big, 1, &mut Meter::new(u1, Limit::Capture)).is_ok());
    assert_eq!(owner_frames(&big, 1, &mut Meter::new(u1 - 1, Limit::Capture)).err(), Some(Refusal::CaptureOps));
}

// ---- F3b: SPU/RSX hot paths over completed pairs (t_d9789c06) ----------------------------------------------------

use ps3dk_prof::foreign_paths::{foreign_paths, FEpochState, FPLimit, FRoot, FSourceState, ForeignPathBudgets, ForeignPathMap};

fn fp_run(name: &str, ext: Vec<Chunk>, b: &ForeignPathBudgets) -> (ExtCapture, PairSink, ForeignPathMap) {
    let x = capture(name, ext);
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let mut ps = PairSink::new(u64::MAX);
    let d = durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let p = foreign_paths(&c, &d, &ps, b);
    (x, ps, p)
}

fn fp_rows(p: &ForeignPathMap) -> &Vec<ps3dk_prof::foreign_paths::FRow> {
    let s = p.sources.iter().find(|s| s.source == SourceKey::Lane(LANE)).expect("SPU lane");
    match &s.state {
        FSourceState::Paths { rows, .. } => rows,
        other => panic!("{other:?}"),
    }
}

fn chain_descs(r: &ps3dk_prof::foreign_paths::FRow) -> Vec<u32> {
    r.key.chain.iter().map(|c| c.1).collect()
}

#[test]
fn fp_parent_index_and_depth_on_a_parsed_three_level_capture() {
    // zone 0 (d0) > zone 1 (d1) > zone 0 (d2); facts are in END order: inner, middle, outer
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (982, 0, 1, 2), (980, 0, 2, 2), (975, 1, 2, 1), (970, 0, 2, 0)]),
    ];
    let (_, ps, p) = fp_run("fp-three", lane_only(body, (1, 6, 6, 0)), &ForeignPathBudgets::default());
    let f = &ps.epochs[&(SourceKey::Lane(LANE), 1)];
    let got: Vec<(Option<u32>, u8, bool)> = f.iter().map(|x| (x.parent, x.depth, x.top_level)).collect();
    assert_eq!(got, vec![(Some(1), 2, false), (Some(2), 1, false), (None, 0, true)]);
    assert!(f[0].begin_seq > f[1].begin_seq && f[1].begin_seq > f[2].begin_seq);
    let mut keys: Vec<Vec<u32>> = fp_rows(&p).iter().map(chain_descs).collect();
    keys.sort();
    assert_eq!(keys, vec![vec![0], vec![0, 1], vec![0, 1, 0]]);
    assert!(fp_rows(&p).iter().all(|r| r.key.root.complete()));
}

#[test]
fn fp_an_open_ancestor_at_eof_gives_a_partial_root_never_merged_with_a_complete_one() {
    // B (desc 1) completed at depth 0; then A (desc 0) BEGIN, B BEGIN/END at depth 1, A never ends (open at EOF)
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(995, 1, 1, 0), (992, 1, 2, 0), (990, 0, 1, 0), (985, 1, 1, 1), (980, 1, 2, 1)]),
    ];
    let (_, _, p) = fp_run("fp-open-eof", lane_fin(body, (1, 5, 5, 0, 1)), &ForeignPathBudgets::default());
    let rows = fp_rows(&p);
    let mut roots: Vec<(FRoot, Vec<u32>)> = rows.iter().map(|r| (r.key.root.clone(), chain_descs(r))).collect();
    roots.sort();
    assert_eq!(roots, vec![(FRoot { missing: 0, gaps: vec![] }, vec![1]), (FRoot { missing: 1, gaps: vec![] }, vec![1])]);
    let partial = rows.iter().find(|r| !r.key.root.complete()).unwrap();
    assert_eq!(partial.key.root.text(), "PARTIAL(missing 1) > ");
}

#[test]
fn fp_a_loss_after_the_child_end_gives_a_partial_root() {
    // A BEGIN (d0), B BEGIN/END (d1), then a LOSS censors A
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (980, 1, 2, 1)]);
    ext.insert(6, loss(LANE, 1, 3, 1));
    ext[8] = finals(&[(LANE, 1, 1, 4, 3, 1, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let (_, _, p) = fp_run("fp-loss", ext, &ForeignPathBudgets::default());
    let rows = fp_rows(&p);
    assert_eq!(rows.len(), 1, "{rows:?}");
    assert_eq!((rows[0].key.root.clone(), chain_descs(&rows[0])), (FRoot { missing: 1, gaps: vec![] }, vec![1]));
}

#[test]
fn fp_an_excluded_parent_keeps_its_child_under_it() {
    // the parent ends outside the calibration range (extrapolated): counted, not summed; the child stays under it
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(950, 0, 1, 0), (945, 1, 1, 1), (940, 1, 2, 1), (850, 0, 2, 0)])];
    let (_, _, p) = fp_run("fp-excl-parent", lane_only(body, (1, 4, 4, 0)), &ForeignPathBudgets::default());
    let rows = fp_rows(&p);
    let parent = rows.iter().find(|r| chain_descs(r) == vec![0]).unwrap();
    let child = rows.iter().find(|r| chain_descs(r) == vec![0, 1]).expect("the child keeps its excluded parent");
    assert_eq!((parent.counts.extrapolated, parent.counts.included), (1, 0));
    assert_eq!(parent.exclusive.hi, Some(ps3dk_prof::clockmap::Rat::int(0)), "not summed");
    assert_eq!(child.counts.included, 1);
}

#[test]
fn fp_one_epoch_sum_equals_the_zone_exclusive_bound() {
    let (_, _, p) = fp_run("fp-nested", f2_nested(), &ForeignPathBudgets::default());
    let (_, _, d) = du("fp-nested-du", f2_nested());
    let z = summaries(&d, 1).1;
    let top = fp_rows(&p).iter().find(|r| chain_descs(r) == vec![0]).unwrap();
    assert_eq!(btext(&top.exclusive), btext(&zone_of(z, 0).exclusive.as_ref().unwrap().b_sum));
    let child = fp_rows(&p).iter().find(|r| chain_descs(r) == vec![0, 1]).unwrap();
    assert_eq!(btext(&child.exclusive), btext(&zone_of(z, 1).exclusive.as_ref().unwrap().b_sum));
}

#[test]
fn fp_synthetic_invalid_parents_and_gap_refuse_or_mark_the_epoch() {
    // SYNTHETIC: the reader never yields these; they are made by rewriting retained facts.
    let x = capture("fp-synth", lane_only(vec![du_cal(), spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (980, 1, 2, 1), (970, 0, 2, 0)])], (1, 4, 4, 0)));
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let mut ps = PairSink::new(u64::MAX);
    let d = durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let key = (SourceKey::Lane(LANE), 1);
    let state = |ps: &PairSink| {
        let p = foreign_paths(&c, &d, ps, &ForeignPathBudgets::default());
        p.sources.iter().find(|s| s.source == SourceKey::Lane(LANE)).unwrap().epochs[0].state.clone()
    };
    assert_eq!(state(&ps), FEpochState::Summed);
    for (bad, what) in [(Some(9u32), "out of range"), (Some(0u32), "self"), (Some(u32::MAX), "overflow marker")] {
        let mut q = PairSink::new(u64::MAX);
        q.epochs = ps.epochs.clone();
        q.epochs.get_mut(&key).unwrap()[0].parent = bad;
        assert_eq!(state(&q), FEpochState::Refused(FPLimit::InvalidParent), "{what}");
    }
    // BEGIN order violated (the parent's BEGIN after the child's)
    let mut q = PairSink::new(u64::MAX);
    q.epochs = ps.epochs.clone();
    let v = q.epochs.get_mut(&key).unwrap();
    v[1].begin_seq = v[0].begin_seq + 100;
    assert_eq!(state(&q), FEpochState::Refused(FPLimit::InvalidParent), "BEGIN order");
    // a depth jump inside the chain (a censored middle pair): a PARTIAL gap root, never merged
    let mut q = PairSink::new(u64::MAX);
    q.epochs = ps.epochs.clone();
    q.epochs.get_mut(&key).unwrap()[0].depth = 2;
    let p = foreign_paths(&c, &d, &q, &ForeignPathBudgets::default());
    assert!(fp_rows(&p).iter().any(|r| r.key.root == FRoot { missing: 0, gaps: vec![(1, 1)] }), "{p:?}");
}

#[test]
fn fp_depth_cap_exact_and_one_over_is_counted() {
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (982, 0, 1, 2), (980, 0, 2, 2), (975, 1, 2, 1), (970, 0, 2, 0)]),
    ];
    let ext = lane_only(body, (1, 6, 6, 0));
    let (_, _, at) = fp_run("fp-cap-at", ext.clone(), &ForeignPathBudgets { max_path_depth: 3, ..ForeignPathBudgets::default() });
    let (_, _, over) = fp_run("fp-cap-over", ext, &ForeignPathBudgets { max_path_depth: 2, ..ForeignPathBudgets::default() });
    let deep = |p: &ForeignPathMap| match &p.sources.iter().find(|s| s.source == SourceKey::Lane(LANE)).unwrap().state {
        FSourceState::Paths { too_deep, rows, .. } => (*too_deep, rows.len()),
        other => panic!("{other:?}"),
    };
    assert_eq!(deep(&at), (0, 3));
    assert_eq!(deep(&over), (1, 2));
}

#[test]
fn fp_capture_ops_exact_and_one_short_refuse_the_last_source() {
    let (_, _, full) = fp_run("fp-ops", cm_ext(), &ForeignPathBudgets::default());
    assert!(full.exhausted_at.is_none());
    let (_, _, at) = fp_run("fp-ops-at", cm_ext(), &ForeignPathBudgets { capture_ops: full.used, ..ForeignPathBudgets::default() });
    assert_eq!(at, full);
    let (_, _, short) = fp_run("fp-ops-short", cm_ext(), &ForeignPathBudgets { capture_ops: full.used - 1, ..ForeignPathBudgets::default() });
    let last = short.sources.last().unwrap();
    assert_eq!(last.state, FSourceState::Refused(FPLimit::CaptureOps));
    assert!(last.epochs.iter().all(|e| matches!(e.state, FEpochState::NotComputed(_) | FEpochState::NotMapped(_))), "{last:?}");
    assert!(matches!(short.sources[0].state, FSourceState::Paths { .. }), "the earlier source is kept whole");
}

// ---- SPU/RSX timeline rows (director 2026-11-04 19:32) -----------------------------------------------------------

use ps3dk_prof::clockmap::{End, Projection, Rat};
use ps3dk_prof::foreign_timeline::{
    end_cmp, foreign_timeline, has_core, FtBar, FtEpoch, FtLimit, FtSourceState, ForeignTimeline, TimelineBudgets,
};

fn ft_run(name: &str, ext: Vec<Chunk>, p: Premises, b: &TimelineBudgets) -> (ModelSink, PairSink, ForeignTimeline) {
    let x = capture(name, ext);
    let mut ms = ModelSink::new(u64::MAX);
    let c = ps3dk_prof::clockmap::map_capture_models(&x, p, &Budgets::default(), &mut ms);
    let mut ps = PairSink::new(u64::MAX);
    let d = durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let t = foreign_timeline(&c, &d, &ms, &ps, b);
    (ms, ps, t)
}

fn ft_bars(t: &ForeignTimeline, key: SourceKey) -> &Vec<FtBar> {
    match &t.sources.iter().find(|s| s.source == key).expect("source").state {
        FtSourceState::Bars(v) => v,
        other => panic!("{other:?}"),
    }
}

fn at(n: i128, d: i128, open: bool) -> End {
    End::At { value: Rat { num: n.into(), den: d.into() }, open }
}

fn bar(b: (End, End), e: (End, End)) -> FtBar {
    FtBar {
        epoch: 1,
        image: None,
        desc: 0,
        depth: 0,
        begin: Projection { lo: b.0, hi: b.1, interpolated: true },
        end: Projection { lo: e.0, hi: e.1, interpolated: true },
        inclusive: 1,
        exclusive: Some(1),
        extrapolated: false,
    }
}

#[test]
fn ft_every_completed_pair_is_placed_with_the_models_exact_projections() {
    let (ms, ps, t) = ft_run("ft-cm", cm_ext(), NOWRAP, &TimelineBudgets::default());
    assert!(t.exhausted_at.is_none() && !t.retention_refused);
    for s in &t.sources {
        let key = (s.source, 1);
        let facts = &ps.epochs[&key];
        let FtSourceState::Bars(bars) = &s.state else { panic!("{s:?}") };
        assert_eq!(bars.len(), facts.len(), "{:?}", s.source);
        assert_eq!(s.counts.placed, facts.len() as u64);
        let model = &ms.models[&key];
        for f in facts {
            let mut m = Meter::unlimited();
            let (b, e) = (model.project(&mut m, f.fb).unwrap(), model.project(&mut m, f.fe).unwrap());
            assert!(bars.iter().any(|x| x.begin == b && x.end == e && x.desc == f.desc), "{f:?}");
        }
        assert!(bars.windows(2).all(|w| end_cmp(&w[0].begin.lo, &w[1].begin.lo) != std::cmp::Ordering::Greater));
    }
}

#[test]
fn ft_core_is_decided_by_exact_finite_bounds_only() {
    let p = |n| at(n, 3, false);
    assert!(!has_core(&bar((p(1), p(2)), (p(2), p(4)))), "touching: no core");
    assert!(!has_core(&bar((p(1), p(3)), (p(2), p(4)))), "overlapping endpoint intervals: no core");
    assert!(has_core(&bar((p(1), p(2)), (p(3), p(4)))));
    assert!(has_core(&bar((End::NegInf, p(2)), (p(3), End::PosInf))), "unbounded OUTER sides keep a finite core");
    assert!(!has_core(&bar((p(1), End::PosInf), (p(3), p(4)))), "unbounded inner side: no core");
    assert!(!has_core(&bar((p(1), p(2)), (End::NegInf, p(4)))));
    assert!(has_core(&bar((p(1), at(2, 3, true)), (p(3), p(4)))), "an OPEN finite end is finite");
    assert_eq!(end_cmp(&at(2, 3, true), &End::PosInf), std::cmp::Ordering::Less, "open finite is not infinity");
    assert_eq!(end_cmp(&at(2, 3, true), &at(4, 6, false)), std::cmp::Ordering::Equal, "value order ignores openness");
    let big = |v: i128| at(v, 1, false);
    let h = 1i128 << 62;
    assert!(has_core(&bar((big(h), big(h + 1)), (big(h + 2), big(h + 3)))), "huge ticks, 1-tick gap");
    assert!(!has_core(&bar((big(h), big(h + 2)), (big(h + 2), big(h + 3)))));
}

#[test]
fn ft_depth_rows_cap_counts_deeper_pairs() {
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (982, 0, 1, 2), (980, 0, 2, 2), (975, 1, 2, 1), (970, 0, 2, 0)]),
    ];
    let ext = lane_only(body, (1, 6, 6, 0));
    let (_, _, all) = ft_run("ft-depth-all", ext.clone(), NOWRAP, &TimelineBudgets { max_depth_rows: 3, ..TimelineBudgets::default() });
    let (_, _, cut) = ft_run("ft-depth-cut", ext, NOWRAP, &TimelineBudgets { max_depth_rows: 2, ..TimelineBudgets::default() });
    let key = SourceKey::Lane(LANE);
    assert_eq!(ft_bars(&all, key).len(), 3);
    assert_eq!(ft_bars(&cut, key).len(), 2);
    let s = cut.sources.iter().find(|s| s.source == key).unwrap();
    assert_eq!((s.counts.placed, s.counts.too_deep), (2, 1));
    assert_eq!(s.epochs[0].state, FtEpoch::Placed { bars: 2, too_deep: 1 });
}

#[test]
fn ft_capture_ops_exact_and_one_short_refuse_the_current_and_later_sources() {
    let (_, _, full) = ft_run("ft-ops", cm_ext(), NOWRAP, &TimelineBudgets::default());
    assert_eq!(full.sources.len(), 2);
    let run = |ops| ft_run("ft-ops-b", cm_ext(), NOWRAP, &TimelineBudgets { capture_ops: ops, ..TimelineBudgets::default() }).2;
    assert_eq!(run(full.used), full, "exactly the full use suffices");
    let short = run(full.used - 1);
    assert_eq!(short.sources.last().unwrap().state, FtSourceState::Refused(FtLimit::CaptureOps));
    assert!(matches!(short.sources[0].state, FtSourceState::Bars(_)), "the earlier source is kept whole");
    for ops in 0..=full.used {
        let t = run(ops);
        let refused: Vec<bool> = t.sources.iter().map(|s| matches!(s.state, FtSourceState::Refused(_))).collect();
        assert!(refused.windows(2).all(|w| !w[0] || w[1]), "{ops}: {refused:?}");
        for s in &t.sources {
            if let FtSourceState::Refused(_) = s.state {
                assert_eq!(s.counts.placed, 0, "{ops}");
                assert!(s.epochs.iter().all(|r| !matches!(r.state, FtEpoch::Placed { .. })), "{ops}");
            }
        }
    }
}

#[test]
fn ft_retained_bars_exact_and_one_over() {
    let (_, _, full) = ft_run("ft-ret", cm_ext(), NOWRAP, &TimelineBudgets::default());
    let total: u64 = full.sources.iter().map(|s| s.counts.placed).sum();
    let run = |n| ft_run("ft-ret-b", cm_ext(), NOWRAP, &TimelineBudgets { retained_bars: n, ..TimelineBudgets::default() }).2;
    let exact = run(total);
    assert!(exact.sources.iter().all(|s| matches!(s.state, FtSourceState::Bars(_))));
    let over = run(total - 1);
    assert_eq!(over.sources.last().unwrap().state, FtSourceState::Refused(FtLimit::RetainedBars));
    assert!(matches!(over.sources[0].state, FtSourceState::Bars(_)));
}

#[test]
fn ft_an_unmapped_epoch_has_coverage_and_no_bars() {
    let (_, _, t) = ft_run("ft-ambig", cm_ext(), Premises { spu_nowrap: false }, &TimelineBudgets::default());
    let s = t.sources.iter().find(|s| s.source == SourceKey::Lane(LANE)).unwrap();
    assert_eq!(s.epochs[0].state, FtEpoch::AmbiguousLift);
    assert_eq!(ft_bars(&t, SourceKey::Lane(LANE)).len(), 0);
    assert_eq!(s.counts.placed, 0);
    assert!(!ft_bars(&t, SourceKey::Gpu(RSX_D)).is_empty(), "the RSX domain is unaffected");
}

#[test]
fn ft_extrapolated_and_exclusive_unavailable_pairs_are_kept_and_marked() {
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(950, 0, 1, 0), (850, 0, 2, 0)])];
    let (_, _, t) = ft_run("ft-extrap", lane_only(body, (1, 2, 2, 0)), NOWRAP, &TimelineBudgets::default());
    let b = &ft_bars(&t, SourceKey::Lane(LANE))[0];
    assert!(b.extrapolated && !b.end.interpolated, "{b:?}");
    assert_eq!(t.sources[0].counts.extrapolated, 1);
    let (_, ps, t) = ft_run("ft-nested", f2_nested(), NOWRAP, &TimelineBudgets::default());
    let facts = &ps.epochs[&(SourceKey::Lane(LANE), 1)];
    let bars = ft_bars(&t, SourceKey::Lane(LANE));
    for f in facts {
        assert!(bars.iter().any(|x| x.exclusive == f.exclusive && x.inclusive == f.inclusive && x.depth == f.depth), "{f:?}");
    }
}

#[test]
fn ft_loss_is_counted_in_records_never_as_pairs() {
    let mut ext = std_ext();
    ext[5] = spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (980, 1, 2, 1)]);
    ext.insert(6, loss(LANE, 1, 3, 1));
    ext[8] = finals(&[(LANE, 1, 1, 4, 3, 1, 0), (RSX_D, 2, 1, 2, 2, 0, 0)]);
    let (_, _, t) = ft_run("ft-loss", ext, NOWRAP, &TimelineBudgets::default());
    let s = t.sources.iter().find(|s| s.source == SourceKey::Lane(LANE)).unwrap();
    assert_eq!(s.counts.placed, 1, "only the completed pair: {s:?}");
    assert!(s.counts.censored >= 1, "the cut ancestor is an event count: {s:?}");
}

#[test]
fn ft_retention_refusal_places_nothing() {
    let x = capture("ft-keep", cm_ext());
    let mut ms = ModelSink::new(0);
    let c = ps3dk_prof::clockmap::map_capture_models(&x, NOWRAP, &Budgets::default(), &mut ms);
    let mut ps = PairSink::new(u64::MAX);
    let d = durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let t = foreign_timeline(&c, &d, &ms, &ps, &TimelineBudgets::default());
    assert!(t.retention_refused && t.sources.is_empty(), "{t:?}");
}

#[test]
fn ft_spu_lane_and_rsx_domain_stay_distinct_sources() {
    let (_, _, t) = ft_run("ft-keys", cm_ext(), NOWRAP, &TimelineBudgets::default());
    let keys: Vec<SourceKey> = t.sources.iter().map(|s| s.source).collect();
    assert!(keys.contains(&SourceKey::Lane(LANE)) && keys.contains(&SourceKey::Gpu(RSX_D)), "{keys:?}");
}

#[test]
fn ft_charges_match_the_independent_cost_model() {
    use ps3dk_prof::clockmap::clog2;
    let (ms, ps, t) = ft_run("ft-model", cm_ext(), NOWRAP, &TimelineBudgets::default());
    let mut want = 0u64;
    for s in &t.sources {
        let key = (s.source, 1);
        let facts = &ps.epochs[&key];
        assert_eq!(facts.len(), 1, "one pair per source in cm_ext");
        let f = &facts[0];
        // projection: the model's own charge for the distinct coordinates (two, fb != fe)
        assert_ne!(f.fb, f.fe);
        let mut pm = Meter::unlimited();
        let model = &ms.models[&key];
        let mut qs = vec![f.fb, f.fe];
        qs.sort();
        for q in qs {
            model.project(&mut pm, q).unwrap();
        }
        let lookups = (1 + clog2(ms.models.len() as u64 + 1)) + (1 + clog2(ps.epochs.len() as u64 + 1));
        let collect = 2 * (1 + clog2(1));
        let publish = 2 * (1 + clog2(3)) + 1;
        let sort = 2 + 1; // n = 1: two slot vectors, no comparison, the final permutation
        want += 1 + lookups + 1 + collect + pm.used() + publish + sort;
    }
    assert_eq!(t.used, want);
}

#[test]
fn ft_a_zero_retained_limit_projects_and_allocates_nothing() {
    use ps3dk_prof::clockmap::clog2;
    let (ms, ps, t) = ft_run("ft-ret0", cm_ext(), NOWRAP, &TimelineBudgets { retained_bars: 0, ..TimelineBudgets::default() });
    // the first source: traversal, the two lookups, the eligibility count of its one fact; then refused (and every
    // later source with it, uncharged)
    let lookups = (1 + clog2(ms.models.len() as u64 + 1)) + (1 + clog2(ps.epochs.len() as u64 + 1));
    assert_eq!(t.used, 1 + lookups + 1);
    assert!(t.sources.iter().all(|s| s.state == FtSourceState::Refused(FtLimit::RetainedBars)), "{t:?}");
}

#[test]
fn ft_sort_refuses_bits_and_large_sorts_stay_within_the_model() {
    use ps3dk_prof::clockmap::{clog2, Meter};
    use ps3dk_prof::foreign_timeline::sort_bars;
    let big = |shift: usize, den_shift: usize| End::At {
        value: Rat { num: num_bigint::BigInt::from(1) << shift, den: (num_bigint::BigInt::from(1) << den_shift) + 1 },
        open: false,
    };
    let a = bar((big(500, 1), big(500, 1)), (big(501, 1), big(501, 1)));
    let b = bar((big(10, 40), big(10, 40)), (big(11, 40), big(11, 40)));
    assert_eq!(sort_bars(&mut Meter::unlimited(), vec![a, b]).err(), Some(FtLimit::Bits));
    // 4096 bars in reverse order: comparisons <= n * clog2(n), each at most 1 + 3 + 3
    let n = 4096u64;
    let v: Vec<FtBar> = (0..n as i128).rev().map(|i| bar((at(i, 1, false), at(i, 1, false)), (at(i + 1, 1, false), at(i + 1, 1, false)))).collect();
    let mut m = Meter::unlimited();
    let out = sort_bars(&mut m, v).unwrap();
    assert!(out.windows(2).all(|w| end_cmp(&w[0].begin.lo, &w[1].begin.lo) == std::cmp::Ordering::Less));
    let comps_max = n * clog2(n);
    let upper = 2 * n + comps_max * (1 + 1 + 3 + 3) + clog2(n) * n + n;
    assert!(m.used() <= upper && m.used() >= 3 * n, "{} not in [{}, {upper}]", m.used(), 3 * n);
}

#[test]
fn ft_event_counts_carry_their_scope() {
    let (_, _, t) = ft_run("ft-scope", cm_ext(), Premises { spu_nowrap: false }, &TimelineBudgets::default());
    let spu = t.sources.iter().find(|s| s.source == SourceKey::Lane(LANE)).unwrap();
    assert_eq!((spu.counts.inventory_epochs, spu.counts.no_inventory_epochs), (0, 1), "{spu:?}");
    let rsx = t.sources.iter().find(|s| s.source == SourceKey::Gpu(RSX_D)).unwrap();
    assert_eq!((rsx.counts.inventory_epochs, rsx.counts.no_inventory_epochs), (1, 0), "{rsx:?}");
}

// ---- F3b r3: cross-epoch, collisions, population, unknown counts, budgets ----------------------------------------

fn fp_two_epochs(second_desc: u16) -> Vec<Chunk> {
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (980, 0, 2, 0)]),
        epochs(&[(SPU_D, 2, 2, 0xFFFF_FFFF)]),
        calibration(SPU_D, 2, 0, &[(0xFFFF_1000, 500, 510), (0xFFFE_0000, 600, 700)]),
        spu_events(LANE, 2, 2, &[(0xFFFF_0000, second_desc, 1, 0), (0xFFFE_8000, second_desc, 2, 0)]),
    ];
    lane_only(body, (1, 4, 4, 0))
}

fn rat_add(a: &ps3dk_prof::clockmap::Rat, b: &ps3dk_prof::clockmap::Rat) -> (num_bigint::BigInt, num_bigint::BigInt) {
    use num_integer::Integer;
    let n = &a.num * &b.den + &b.num * &a.den;
    let d = &a.den * &b.den;
    let g = n.gcd(&d);
    (n / &g, d / &g)
}

#[test]
fn fp_one_path_over_two_epochs_is_their_interval_sum_and_spans_n() {
    let (_, _, p) = fp_run("fp-x-epoch", fp_two_epochs(0), &ForeignPathBudgets::default());
    let rows = fp_rows(&p);
    assert_eq!(rows.len(), 1, "{rows:?}");
    let r = &rows[0];
    assert_eq!((r.epochs, r.counts.pairs), (2, 2));
    // each epoch alone (the same path in a one-epoch capture would be that epoch's row)
    let (_, _, d) = du("fp-x-epoch-du", fp_two_epochs(0));
    let (b1, b2) = (zone_of(summaries(&d, 1).1, 0).exclusive.clone().unwrap().b_sum, zone_of(summaries(&d, 2).1, 0).exclusive.clone().unwrap().b_sum);
    let lo = rat_add(&b1.lo, &b2.lo);
    use num_integer::Integer;
    let g = r.exclusive.lo.num.gcd(&r.exclusive.lo.den);
    assert_eq!((&r.exclusive.lo.num / &g, &r.exclusive.lo.den / &g), lo, "lo is the sum of the epochs' lo");
    match (&r.exclusive.hi, &b1.hi, &b2.hi) {
        (Some(h), Some(x), Some(y)) => {
            let g = h.num.gcd(&h.den);
            assert_eq!((&h.num / &g, &h.den / &g), rat_add(x, y));
        }
        (None, _, _) => assert!(b1.hi.is_none() || b2.hi.is_none()),
        other => panic!("{other:?}"),
    }
}

#[test]
fn fp_the_path_cap_jointly_over_epochs_refuses_the_source() {
    let cap = ForeignPathBudgets { max_paths_per_source: 1, ..ForeignPathBudgets::default() };
    let (_, _, one) = fp_run("fp-cap-joint-ok", fp_two_epochs(0), &cap);
    assert!(matches!(one.sources[0].state, FSourceState::Paths { .. }), "one path in both epochs fits");
    let (_, _, two) = fp_run("fp-cap-joint", fp_two_epochs(1), &cap);
    let s = &two.sources[0];
    assert_eq!(s.state, FSourceState::Refused(FPLimit::PathsPerSource));
    assert!(s.epochs.iter().all(|e| e.state == FEpochState::NotComputed(FPLimit::PathsPerSource)), "{s:?}");
    assert!(s.epochs.iter().all(|e| e.pairs == Some(1)), "known inventory counts stay: {s:?}");
    // a later source is NOT refused by a path cap (only the capture budget refuses later sources)
    assert!(two.exhausted_at.is_none());
}

#[test]
fn fp_the_same_descriptor_in_another_image_is_another_path() {
    let body = vec![du_cal(), spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 0, 2, 0), (980, 0, 1, 0), (975, 0, 2, 0)])];
    let x = capture("fp-collide", lane_only(body, (1, 4, 4, 0)));
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let mut ps = PairSink::new(u64::MAX);
    let d = durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let p = foreign_paths(&c, &d, &ps, &ForeignPathBudgets::default());
    assert_eq!(fp_rows(&p).len(), 1, "same image and descriptor: one path");
    // SYNTHETIC: the second pair rewritten to image 1
    let mut q = PairSink::new(u64::MAX);
    q.epochs = ps.epochs.clone();
    q.epochs.get_mut(&(SourceKey::Lane(LANE), 1)).unwrap()[1].image = Some(1);
    let p = foreign_paths(&c, &d, &q, &ForeignPathBudgets::default());
    let keys: Vec<_> = fp_rows(&p).iter().map(|r| r.key.chain.clone()).collect();
    assert_eq!(keys.len(), 2, "{keys:?}");
    // and an SPU lane and an RSX domain never share rows (separate sources)
    let (_, _, cm) = fp_run("fp-collide-src", cm_ext(), &ForeignPathBudgets::default());
    assert_eq!(cm.sources.len(), 2);
}

#[test]
fn fp_population_reconciles_with_retained_rows_and_the_depth_cap() {
    let body = vec![
        du_cal(),
        spu_events(LANE, 1, 0, &[(990, 0, 1, 0), (985, 1, 1, 1), (982, 0, 1, 2), (980, 0, 2, 2), (975, 1, 2, 1), (970, 0, 2, 0)]),
    ];
    let (_, _, p) = fp_run("fp-pop", lane_only(body, (1, 6, 6, 0)), &ForeignPathBudgets { max_path_depth: 2, ..ForeignPathBudgets::default() });
    match &p.sources[0].state {
        FSourceState::Paths { population, counts, too_deep, .. } => {
            assert_eq!((population.pairs, counts.pairs, *too_deep), (3, 2, 1));
            assert_eq!(population.pairs, counts.pairs + too_deep);
            assert_eq!(population.included + population.extrapolated + population.exclusive_unavailable, population.pairs);
        }
        other => panic!("{other:?}"),
    }
}

#[test]
fn fp_epochs_without_an_inventory_report_unknown_pairs_never_zero() {
    let x = capture("fp-unknown", cm_ext());
    let c = map_capture(&x, Premises { spu_nowrap: false }, &Budgets::default());
    let mut ps = PairSink::new(u64::MAX);
    let d = durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let p = foreign_paths(&c, &d, &ps, &ForeignPathBudgets::default());
    let spu = p.sources.iter().find(|s| s.source == SourceKey::Lane(LANE)).unwrap();
    assert!(matches!(spu.epochs[0].state, FEpochState::NotMapped(_)));
    assert_eq!(spu.epochs[0].pairs, None);
}

#[test]
fn fp_budget_zero_refuses_every_source_and_keeps_known_counts() {
    let (_, _, p) = fp_run("fp-zero", cm_ext(), &ForeignPathBudgets { capture_ops: 0, ..ForeignPathBudgets::default() });
    assert_eq!(p.sources.len(), 2);
    assert_eq!(p.exhausted_at, Some(p.sources[0].source));
    for s in &p.sources {
        assert_eq!(s.state, FSourceState::Refused(FPLimit::CaptureOps), "{s:?}");
        assert!(s.epochs.iter().all(|e| e.state == FEpochState::NotComputed(FPLimit::CaptureOps)), "{s:?}");
        assert!(s.epochs.iter().all(|e| e.pairs.is_some()), "every epoch here has an inventory: {s:?}");
    }
}

#[test]
fn fp_every_budget_refuses_monotonically_and_never_publishes_a_partial_source() {
    let (_, _, full) = fp_run("fp-sweep", cm_ext(), &ForeignPathBudgets::default());
    let mut last_refused = 0usize;
    for ops in 0..=full.used {
        let (_, _, p) = fp_run("fp-sweep-b", cm_ext(), &ForeignPathBudgets { capture_ops: ops, ..ForeignPathBudgets::default() });
        let refused: Vec<bool> = p.sources.iter().map(|s| matches!(s.state, FSourceState::Refused(_))).collect();
        assert!(refused.windows(2).all(|w| !w[0] || w[1]), "{ops}: once refused, later sources too: {refused:?}");
        let n = refused.iter().filter(|&&r| r).count();
        assert!(ops == 0 || n <= last_refused, "{ops}: more budget never refuses more");
        last_refused = n;
        for (s, f) in p.sources.iter().zip(&full.sources) {
            let (got, want): (Vec<u32>, Vec<u32>) = (s.epochs.iter().map(|e| e.epoch).collect(), f.epochs.iter().map(|e| e.epoch).collect());
            assert_eq!(got, want, "{ops}: every epoch listed exactly once, in order");
            if let FSourceState::Paths { .. } = s.state {
                assert_eq!(s, f, "{ops}: a published source is whole and equal to the full run");
            }
        }
    }
    assert_eq!(last_refused, 0);
}

#[test]
fn fp_the_pair_fact_lookup_is_charged_by_the_map_size_including_empty_fact_epochs() {
    use ps3dk_prof::clockmap::clog2;
    let x = capture("fp-lookup", cm_ext());
    let c = map_capture(&x, NOWRAP, &Budgets::default());
    let mut ps = PairSink::new(u64::MAX);
    let d = durations_pairs(&x, &c, &DurationBudgets::default(), &mut ps);
    let lookups = 2u64; // one summed epoch per source under W32
    let grow = |base: &PairSink, k: u32| {
        let mut q = PairSink::new(u64::MAX);
        q.epochs = base.epochs.clone();
        for e in 0..k {
            q.epochs.insert((SourceKey::Lane(9_999), e), Vec::new()); // SYNTHETIC: unrelated entries
        }
        q
    };
    for base in [ps.epochs.clone(), {
        // the RSX epoch with an EMPTY fact vector: its lookup is still charged
        let mut v = ps.epochs.clone();
        *v.get_mut(&(SourceKey::Gpu(RSX_D), 1)).unwrap() = Vec::new();
        v
    }] {
        let mut small = PairSink::new(u64::MAX);
        small.epochs = base;
        let a = foreign_paths(&c, &d, &small, &ForeignPathBudgets::default());
        let big = grow(&small, 1000);
        let b = foreign_paths(&c, &d, &big, &ForeignPathBudgets::default());
        let (n0, n1) = (small.epochs.len() as u64, big.epochs.len() as u64);
        assert_eq!(b.used - a.used, lookups * (clog2(n1 + 1) - clog2(n0 + 1)), "the lookup charge alone differs");
        assert_eq!(a.sources.iter().map(|s| &s.state).collect::<Vec<_>>(), b.sources.iter().map(|s| &s.state).collect::<Vec<_>>());
    }
}
