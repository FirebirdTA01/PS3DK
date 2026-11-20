//! C semantic checkpoint parity, explicitly invoked after the prebuilt fixture.
//! The ordinary source is checked FIRST; no Rust checkpoints are passed to C.
#[allow(dead_code)]
mod common;
mod spu_support;
use std::collections::BTreeMap;
use std::io::Read;
use std::path::Path;
use ps3dk_prof::{Outcome, rolling_combined as combined, rolling_file, rolling_store, spu_ticks};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::ext::rolling::causal::{Bounds, CausalLimits, CounterHistory, PlacementBasis};
use ps3dk_prof::spu_ticks::ObsKind;

fn read_bounded(path: &Path, limit: usize) -> Vec<u8> {
    let file = std::fs::File::open(path).unwrap_or_else(|e| panic!("open required artifact {}: {e}", path.display()));
    assert!(file.metadata().unwrap().len() <= limit as u64, "artifact over cap: {}", path.display());
    let mut bytes = Vec::new(); file.take(limit as u64 + 1).read_to_end(&mut bytes).unwrap();
    assert!(bytes.len() <= limit, "artifact grew over cap: {}", path.display()); bytes
}
fn u32_at(bytes: &[u8], at: usize) -> u32 { u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) }
fn u64_at(bytes: &[u8], at: usize) -> u64 { u64::from_le_bytes(bytes[at..at + 8].try_into().unwrap()) }
fn num(row: &serde_json::Value, key: &str) -> u64 { row[key].as_u64().unwrap_or_else(|| panic!("manifest missing numeric {key}")) }

#[test]
#[ignore = "requires independent C source/state/store artifacts via PS3DK_PROF_ROLLING_COMBINED_FIXTURE_DIR"]
fn c_accepted_output_mixed_checkpoints_and_store_match_original_source() {
    let dir = std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_FIXTURE_DIR")
        .expect("set PS3DK_PROF_ROLLING_COMBINED_FIXTURE_DIR to run_combined_fixture.py's output directory");
    let dir = std::fs::canonicalize(dir).expect("required C fixture directory must exist");
    assert!(dir.is_dir());
    let source = read_bounded(&dir.join("source.prof"), 65536);
    // Deliberately before reading checkpoint/store artifacts. A helper-source
    // failure must never masquerade as checkpoint implementation RED.
    let full = match common::load(&source) {
        Outcome::Analysed(a) => a,
        Outcome::Malformed(e) => panic!("C original mixed source must pass ordinary reader FIRST: {e}"),
    };
    assert!(full.capture.incomplete.is_empty(), "C original source must have genuine complete finals");
    assert_eq!(full.capture.lanes[&1].written, 12);
    assert_eq!(full.capture.lanes[&2].written, 0);
    assert_eq!(full.capture.spans.len(), 3);
    let full_causal = spu_ticks::analyze(&full, &spu_ticks::Options { table: spu_support::test_table(), ..Default::default() })
        .expect("C full source must support ordinary SPU causal analysis");
    let expected: BTreeMap<u64, Bounds> = full_causal.lanes[0].obs.iter().filter_map(|o| match o.kind {
        ObsKind::Event { seq, .. } => Some((seq, Bounds { lower: o.l, upper_drain: o.u1, upper_bracket: o.u2, upper: o.u })),
        _ => None,
    }).collect();
    assert_eq!(expected.len(), 6);
    for seq in 0..6 {
        let (lower, drain, bracket) = if seq < 4 { (1100, 1250, 1210) } else { (1200, 1350, 1310) };
        assert_eq!(expected[&seq], Bounds { lower: Some(lower), upper_drain: Some(drain), upper_bracket: Some(bracket), upper: Some(bracket) });
    }
    eprintln!("PASS ordinary C source: complete PPU12/SPU6, three spans, exact causal endpoints");

    // Reconstruct original framing and counts directly from the source bytes.
    let mut offsets = vec![16usize]; let mut types = Vec::new(); let mut ppu_prefix = vec![0u64];
    let mut records_prefix = vec![0u64]; let mut spu_prefix = vec![0u64]; let mut meta_prefix = vec![0u64];
    while *offsets.last().unwrap() < source.len() {
        let at = *offsets.last().unwrap(); let ty = u32_at(&source, at);
        let len = u32_at(&source, at + 4) as usize; let end = at + 20 + len;
        assert_eq!(u64_at(&source, at + 8), types.len() as u64);
        assert_eq!(u32_at(&source, end - 4), common::crc32(&source[at..end - 4]));
        ppu_prefix.push(ppu_prefix.last().unwrap() + if ty == 5 { u64::from(u32_at(&source, at + 20)) } else { 0 });
        records_prefix.push(records_prefix.last().unwrap() + u64::from(ty == 5));
        spu_prefix.push(spu_prefix.last().unwrap() + if ty == 13 { u64::from(u32_at(&source, at + 32)) } else { 0 });
        meta_prefix.push(meta_prefix.last().unwrap() + u64::from(!matches!(ty, 5 | 6 | 13)));
        types.push(ty); offsets.push(end);
    }
    assert_eq!(types.len(), 32); assert_eq!(*offsets.last().unwrap(), source.len());
    let manifest: serde_json::Value = serde_json::from_slice(&read_bounded(&dir.join("cuts.json"), 16384)).unwrap();
    assert_eq!(num(&manifest, "schema"), 1);
    let cuts = manifest["cuts"].as_array().expect("manifest cuts"); assert_eq!(cuts.len(), 13);
    let first = [0usize, 1, 2, 3, 4, 5, 9, 12, 14, 16, 22, 27, 31];
    let table: Vec<_> = spu_support::test_table().into_iter().map(|e| e.entry).collect();
    let options = || combined::ReportOptions { spu_profile_table: &table, causal_limits: CausalLimits::default() };
    let mut checkpoints = Vec::new();
    for (i, row) in cuts.iter().enumerate() {
        let n = first[i]; let offset = offsets[n]; let name = format!("cut-{i:03}.ckp");
        assert_eq!(row["file"].as_str(), Some(name.as_str()));
        assert_eq!(row["kind"].as_str(), Some(if i == 12 { "pre_trailer_probe" } else { "segment_start" }));
        assert_eq!((num(row, "next_chunk"), num(row, "prefix_bytes"), num(row, "source_offset")),
            (n as u64, (offset - 16) as u64, offset as u64));
        assert_eq!((num(row, "ppu_written"), num(row, "ppu_record_chunks"), num(row, "spu_events"), num(row, "metadata")),
            (ppu_prefix[n], records_prefix[n], spu_prefix[n], meta_prefix[n]));
        assert_eq!(num(row, "ppu_stack"), u64::from(i == 9));
        assert_eq!(num(row, "spu_stack"), u64::from(i == 9));
        let cp = read_bounded(&dir.join(name), 32768);
        let direct = combined::checkpoint_combined(&source[..offset], combined::Limits::default())
            .unwrap_or_else(|e| panic!("original cut {i} must encode independently: {e:?}"));
        assert_eq!(cp, direct, "C accepted-output checkpoint serializer must match direct original prefix at cut {i}");
        let restored = combined::read_combined_suffix(&cp, &source[offset..], combined::Limits::default(), options())
            .unwrap_or_else(|e| panic!("C checkpoint at cut {i} must preserve genuine suffix: {e:?}"));
        assert!(restored.terminal && !restored.torn_tail && restored.ppu.incomplete.is_empty());
        assert_eq!((restored.first_chunk, restored.next_chunk, restored.prefix_bytes), (n as u64, 32, (offset - 16) as u64));
        let lane = restored.ppu.lanes.get(&1).expect("original owner survives every cut");
        assert_eq!((lane.prefix_written, lane.retained_written), (ppu_prefix[n], 12 - ppu_prefix[n]));
        let spu = &restored.spu.retained.sources[&SourceKey::Lane(1)];
        assert_eq!((spu.prefix_events, spu.retained_events), (spu_prefix[n], 6 - spu_prefix[n]));
        for e in &restored.spu.causal.lanes[&1].endpoints { assert_eq!(e.bounds, expected[&e.seq]); }
        checkpoints.push(cp);
    }
    eprintln!("PASS all 13 C checkpoints match direct original prefixes and preserve suffix finals");

    // Only after source + exact checkpoint parity: actual C physical output.
    let bytes = read_bounded(&dir.join("rolling.profr"), 262144);
    let store_limits = rolling_store::Limits { max_file_bytes: 262144, max_slot_bytes: 65536 };
    let physical = rolling_store::read(&bytes, store_limits).expect("actual C store must validate physically");
    assert_eq!(physical.segments.len(), 3);
    for (i, s) in physical.segments.iter().enumerate() {
        assert_eq!(s.segment, (9 + i) as u64); assert_eq!(s.checkpoint, checkpoints[9 + i]);
        let next = if i == 2 { 32 } else { first[10 + i] };
        assert_eq!(s.payload, &source[offsets[first[9 + i]]..offsets[next]]);
    }
    let joined = rolling_file::read_combined(&bytes, store_limits, combined::Limits::default(), options())
        .unwrap_or_else(|e| panic!("actual C store must join all retained mixed semantics: {e:?}"));
    let v = joined.store_view;
    assert_eq!((v.format_version, v.state, v.reason, v.generation), (2, 2, 0, 14));
    assert_eq!((v.first_segment, v.next_segment, v.retained_segments, v.evicted_segments), (9, 12, 3, 9));
    assert_eq!((v.first_chunk, v.next_chunk, v.evicted_chunks), (16, 32, 16)); assert!(!v.recovered);
    assert_eq!(v.evicted_payload_bytes, (offsets[16] - 16) as u64);
    assert_eq!(v.bound_payload_bytes, (source.len() - 16) as u64);
    let r = joined.retained; assert!(r.terminal && !r.torn_tail && r.ppu.incomplete.is_empty());
    assert_eq!((r.first_chunk, r.next_chunk, r.prefix_bytes), (16, 32, v.evicted_payload_bytes));
    assert_eq!((r.ppu.lanes[&1].prefix_written, r.ppu.lanes[&1].retained_written), (3, 9));
    assert_eq!((r.ppu.lanes[&2].prefix_written, r.ppu.lanes[&2].retained_written), (0, 0));
    assert_eq!(r.ppu.windows.len(), 1); assert!(r.ppu.windows[0].left_clipped);
    for (name, complete, censored, ticks) in [("A", 0, 1, None), ("B", 1, 0, Some(20)), ("C", 1, 0, Some(30))] {
        let z = r.ppu.zones.iter().find(|z| z.name == name).expect("every retained zone must survive");
        assert_eq!((z.completed, z.left_censored, z.total_ticks), (complete, censored, ticks));
    }
    let spu = &r.spu.retained.sources[&SourceKey::Lane(1)];
    assert_eq!((spu.prefix_events, spu.retained_events), (1, 5));
    assert_eq!(spu.left_censored.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(0, 3)]);
    assert_eq!(spu.pairs.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(1, 2), (4, 5)]);
    assert!(r.spu.causal.refusal.is_none());
    assert_eq!(r.spu.causal.basis, PlacementBasis::CausalSupportOnly);
    assert_eq!(r.spu.causal.counter_history, CounterHistory::NotEvaluated);
    let endpoints = &r.spu.causal.lanes[&1].endpoints;
    assert_eq!(endpoints.iter().map(|e| e.seq).collect::<Vec<_>>(), vec![1, 2, 3, 4, 5]);
    for e in endpoints { assert_eq!(e.bounds, expected[&e.seq]); }
}
