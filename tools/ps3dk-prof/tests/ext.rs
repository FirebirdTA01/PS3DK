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
fn t0_flags_one_with_trailer_and_no_domain_is_malformed_for_missing_domain() {
    let mut f = v1_scenario().file();
    f.flags = 1;
    expect_malformed("t0-flags1-nodomain", &f.bytes(), "declares no DOMAIN");
}

#[test]
fn t0_flags_one_torn_before_any_domain_is_incomplete() {
    let mut f = v1_scenario().file().without_trailer();
    f.flags = 1;
    let bytes = f.bytes();
    dump("t0-flags1-torn-nodomain", &bytes);
    match load(&bytes) {
        Outcome::Analysed(a) => assert!(matches!(a.verdict, ps3dk_prof::Verdict::Incomplete(_))),
        o => panic!("{o:?}"),
    }
}

#[test]
fn t0_unknown_flag_bits_are_malformed_in_every_byte() {
    for flags in [2u32, 3, 0x100, 0x10000, 0x8000_0000] {
        let mut f = ext_file(std_ext());
        f.flags = flags;
        dump(&format!("t0-flags-{flags:x}"), &f.bytes());
        match load(&f.bytes()) {
            Outcome::Malformed(m) => assert_eq!(m.rule, Rule::FileFlagsNonzero, "flags {flags:#x}"),
            o => panic!("flags {flags:#x}: {o:?}"),
        }
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
