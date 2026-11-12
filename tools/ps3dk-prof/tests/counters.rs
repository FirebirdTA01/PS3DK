//! Named per-frame counters (file_flags bit 8, chunk 39): independent bytes, no production encoder.
mod common;
use common::*;
use ps3dk_prof::counters::{self, CHUNK_COUNTERS, FLAG_COUNTERS};
use ps3dk_prof::Rule;

/// One COUNTERS payload: header, then (id, name) declarations, then (incarnation, id, frame, tb, value) samples.
fn payload(flags: u16, names: &[(u32, &str)], samples: &[(u32, u32, u64, u64, i64)], dropped: u64, refused: u64) -> Vec<u8> {
    let mut b = Buf::default();
    b.u16(1).u16(flags).u32(names.len() as u32).u32(samples.len() as u32).u32(0).u64(dropped).u64(refused);
    for (id, n) in names {
        b.u32(*id).u16(n.len() as u16);
        b.0.extend_from_slice(n.as_bytes());
    }
    for &(inc, id, frame, tb, v) in samples {
        b.u32(inc).u32(id).u64(frame).u64(tb).u64(v as u64);
    }
    b.0
}

/// A sealed capture: lane 1 with two frames and two zones named like an application's wait/drain zones, a counter
/// chunk after the records and a FINAL one before the TRAILER.
fn sealed(extra_before_trailer: Vec<Chunk>) -> File {
    let mut s = scenario(vec![lane(1, seqd(0, vec![frame(0, 10, 0), frame(0, 20, 1)]))]);
    s.descs = vec![desc(0, "wait.rsx_label", ""), desc(1, "drain.retired", "")];
    let mut f = s.file();
    let trailer = f.chunks.pop().unwrap();
    f.chunks.extend(extra_before_trailer);
    f.chunks.push(trailer);
    f.flags = FLAG_COUNTERS;
    f
}

fn good() -> File {
    sealed(vec![
        chunk(CHUNK_COUNTERS, payload(0, &[(0, "arena.bytes_used"), (1, "arena.fallbacks")],
                                      &[(1, 0, 1, 11, 4096), (1, 1, 1, 12, 0), (1, 0, 2, 21, 8192)], 0, 0)),
        chunk(CHUNK_COUNTERS, payload(1, &[], &[(1, 1, 2, 22, -3)], 1, 2)),
    ])
}

fn rule(f: &File) -> Rule {
    ps3dk_prof::reader::read(&f.bytes()).unwrap_err().rule
}

#[test]
fn a_sealed_counter_capture_decodes_with_names_frames_and_totals() {
    let c = ps3dk_prof::reader::read(&good().bytes()).expect("valid counters capture");
    let inv = c.counters.as_ref().expect("capability");
    assert_eq!(inv.names, vec!["arena.bytes_used", "arena.fallbacks"]);
    assert_eq!(inv.samples.len(), 4);
    assert_eq!(inv.samples[3].value, -3);
    assert!(inv.final_seen);
    assert_eq!((inv.dropped, inv.refused), (1, 2));
    let r = counters::analyze(inv);
    assert_eq!(r.frames.len(), 2);
    assert_eq!(r.summaries[0].max, 8192);
    assert_eq!(r.summaries[1].min, -3);
    let mut text = Vec::new();
    counters::write_text(&mut text, Some(inv)).unwrap();
    let text = String::from_utf8(text).unwrap();
    assert!(text.contains("incarnation 1 frame 0: arena.bytes_used=4096 arena.fallbacks=0"), "{text}");
    assert!(text.contains("incarnation 1 frame 1: arena.bytes_used=8192 arena.fallbacks=-3"), "{text}");
    let mut json = Vec::new();
    counters::write_json(&mut json, Some(inv)).unwrap();
    let j: serde_json::Value = serde_json::from_slice(&json).unwrap();
    assert_eq!(j["names"][0]["samples"], 2);
    assert_eq!(j["samples"][3]["value"], -3);
    // The zone names and frame records are those of the base format, unchanged by the capability.
    let names: Vec<&str> = c.descriptors.values().map(|d| d.name.as_str()).collect();
    assert_eq!(names, vec!["wait.rsx_label", "drain.retired"]);
}

#[test]
fn the_same_zones_read_identically_with_and_without_the_capability() {
    // D4: the base chunks of a counters capture decode exactly as the same capture without the flag.
    let with = ps3dk_prof::reader::read(&good().bytes()).unwrap();
    let mut plain = good();
    plain.chunks.retain(|c| c.ty != CHUNK_COUNTERS);
    plain.flags = 0;
    let without = ps3dk_prof::reader::read(&plain.bytes()).unwrap();
    assert_eq!(format!("{:?}", with.descriptors), format!("{:?}", without.descriptors));
    assert_eq!(format!("{:?}", with.lanes), format!("{:?}", without.lanes));
    assert_eq!(format!("{:?}", with.spans), format!("{:?}", without.spans));
    assert!(without.counters.is_none());
}

#[test]
fn counters_chunks_without_the_capability_are_unknown() {
    let mut f = good();
    f.flags = 0;
    assert_eq!(rule(&f), Rule::UnknownChunkType);
}

#[test]
fn a_reader_that_does_not_know_bit_8_refuses_other_high_bits() {
    let mut f = good();
    f.flags = FLAG_COUNTERS | 512;
    assert_eq!(rule(&f), Rule::FileFlagsNonzero);
    // the partial-header path agrees: byte 13 may be 0 or 1 only
    let bytes = f.bytes();
    assert!(ps3dk_prof::reader::read(&bytes[..14]).is_err());
    let ok = good().bytes();
    let torn = ps3dk_prof::reader::read(&ok[..14]).expect("a torn header with bit 8 is torn, not malformed");
    assert!(torn.counters.is_some());
}

#[test]
fn strict_decode_refuses_each_violation() {
    let cases: Vec<(&str, Vec<u8>)> = vec![
        ("schema", { let mut p = payload(1, &[(0, "a")], &[], 0, 0); p[0] = 2; p }),
        ("flags", payload(2 | 1, &[(0, "a")], &[], 0, 0)),
        ("reserved", { let mut p = payload(1, &[(0, "a")], &[], 0, 0); p[12] = 1; p }),
        ("id order", payload(1, &[(1, "a")], &[], 0, 0)),
        ("empty name", payload(1, &[(0, "")], &[], 0, 0)),
        ("space in name", payload(1, &[(0, "a b")], &[], 0, 0)),
        ("long name", payload(1, &[(0, &"x".repeat(65))], &[], 0, 0)),
        ("duplicate", payload(1, &[(0, "a"), (1, "a")], &[], 0, 0)),
        ("undeclared id", payload(1, &[(0, "a")], &[(1, 1, 0, 1, 0)], 0, 0)),
        ("undeclared incarnation", payload(1, &[(0, "a")], &[(9, 0, 0, 1, 0)], 0, 0)),
        ("trailing byte", { let mut p = payload(1, &[(0, "a")], &[], 0, 0); p.push(0); p }),
        ("short sample", { let mut p = payload(1, &[(0, "a")], &[(1, 0, 0, 1, 0)], 0, 0); p.pop(); p }),
        ("empty non-final", payload(0, &[], &[], 0, 0)),
    ];
    for (what, p) in cases {
        let f = sealed(vec![chunk(CHUNK_COUNTERS, p)]);
        assert_eq!(rule(&f), Rule::Counters, "{what}");
    }
    let too_many: Vec<(u32, String)> = (0..65).map(|i| (i, format!("c{i}"))).collect();
    let refs: Vec<(u32, &str)> = too_many.iter().map(|(i, s)| (*i, s.as_str())).collect();
    assert_eq!(rule(&sealed(vec![chunk(CHUNK_COUNTERS, payload(1, &refs, &[], 0, 0))])), Rule::Counters, "65 names");
}

#[test]
fn whole_file_rules() {
    // a sealed capture needs exactly one FINAL, the last counters chunk
    let no_final = sealed(vec![chunk(CHUNK_COUNTERS, payload(0, &[(0, "a")], &[], 0, 0))]);
    assert_eq!(rule(&no_final), Rule::Counters, "no FINAL");
    let after_final = sealed(vec![
        chunk(CHUNK_COUNTERS, payload(1, &[(0, "a")], &[], 0, 0)),
        chunk(CHUNK_COUNTERS, payload(1, &[], &[(1, 0, 0, 1, 0)], 0, 0)),
    ]);
    assert_eq!(rule(&after_final), Rule::Counters, "chunk after FINAL");
    let decreasing = sealed(vec![
        chunk(CHUNK_COUNTERS, payload(0, &[(0, "a")], &[], 5, 0)),
        chunk(CHUNK_COUNTERS, payload(1, &[], &[], 4, 0)),
    ]);
    assert_eq!(rule(&decreasing), Rule::Counters, "dropped decreased");
    // without a TRAILER the samples are kept and the inventory is not final
    let mut torn = good();
    torn.chunks.pop();
    torn.chunks.pop();
    let c = ps3dk_prof::reader::read(&torn.bytes()).unwrap();
    let inv = c.counters.unwrap();
    assert!(!inv.final_seen);
    assert_eq!(inv.samples.len(), 3);
    // a sample before its incarnation's THREADS chunk is undeclared
    let mut early = good();
    let t = early.find(T_THREADS);
    let first = early.chunks.iter().position(|c| c.ty == CHUNK_COUNTERS).unwrap();
    let moved = early.chunks.remove(first);
    early.chunks.insert(t, moved);
    assert_eq!(rule(&early), Rule::Counters, "sample before THREADS");
}
