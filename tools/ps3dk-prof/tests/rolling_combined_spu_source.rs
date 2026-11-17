//! Source scaffolding only: ordinary reader + producer truth + actual callback
//! bytes. Does not invoke or make any assertion about a rolling encoder.
#[allow(dead_code)]
mod common;
use ps3dk_prof::{Outcome, ext::{OpenBegins, SourceKey, Structure}};
use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};

fn dir() -> PathBuf {
    let p = std::env::var_os("PS3DK_PROF_ROLLING_COMBINED_SOURCE_DIR").expect("set prebuilt source artifact directory");
    let p = std::fs::canonicalize(p).unwrap(); assert!(p.is_dir()); p
}
fn bytes(p: &Path) -> Vec<u8> {
    let f = std::fs::File::open(p).unwrap(); assert!(f.metadata().unwrap().len() <= 1048576);
    let mut b = Vec::new(); f.take(1048577).read_to_end(&mut b).unwrap(); assert!(b.len() <= 1048576); b
}
fn u32_at(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
fn u64_at(b: &[u8], at: usize) -> u64 { u64::from_le_bytes(b[at..at + 8].try_into().unwrap()) }
fn full(b: &[u8]) -> Box<ps3dk_prof::Analysis> {
    match common::load(b) {
        Outcome::Analysed(a) => { assert!(a.capture.incomplete.is_empty(), "ordinary source must complete"); a }
        Outcome::Malformed(m) => panic!("ordinary mixed source must validate first: {m}"),
    }
}

#[test]
#[ignore = "requires independent combined_spu_source artifacts; no encoder dependency"]
fn real_spu_producer_truth_matches_ordinary_mixed_capture() {
    let d = dir(); let b = bytes(&d.join("source.prof")); let a = full(&b);
    assert_eq!(&b[..8], b"PS3DKPRF"); assert_eq!(&b[8..12], &[1, 0, 7, 0]); assert_eq!(u32_at(&b, 12), 519);
    assert_eq!(a.capture.lanes[&1].written, 8); assert_eq!(a.capture.lanes[&2].written, 5);
    assert_eq!(a.capture.spans.len(), 3); assert!(a.capture.spans.iter().all(|s| s.end_seq.is_some()));
    assert_eq!(a.capture.windows.len(), 1); assert!(a.capture.windows_flag);
    let profile = a.capture.profile.as_ref().unwrap();
    assert_eq!(profile.profile_id, "SPU-MODEL"); assert_eq!(profile.digest, [0x5a; 32]);
    let ext = a.capture.ext.as_ref().unwrap();
    assert_eq!((ext.domains.len(), ext.lanes.len(), ext.images.len(), ext.sources.len()), (3, 3, 1, 3));
    assert_eq!(ext.images[&0].build_id, b"\x02model-image"); assert_eq!(ext.images[&0].descriptors.len(), 3);
    let expected_pairs: [&[(u64, u64)]; 3] = [&[(0, 3), (1, 2)], &[(1, 2)], &[(0, 1)]];
    let mut actual = BTreeMap::new();
    for id in 1..=3u32 {
        let i = id as usize - 1;
        let info = &ext.lanes[&id]; assert_eq!(info.image_index, 0); assert_eq!(info.domain_id, id + 1);
        assert_eq!(info.frame_track_owner, if id == 3 { 2 } else { 1 });
        let s = ext.sources.iter().find(|s| s.key == SourceKey::Lane(id)).unwrap();
        assert_eq!(s.structure, Structure::Complete); assert!(s.raw_valid && s.invalid.is_empty());
        assert!(s.coverage_unknown.is_empty() && s.protocol_error.is_none()); assert_eq!(s.open_begins, OpenBegins::Verified(0));
        assert_eq!(s.events_total, [5, 3, 2][i]); assert_eq!(s.lost_total, 0); assert_eq!(s.epochs, [2, 1, 1][i]);
        assert!(s.raw.losses.is_empty() && s.raw.censored.is_empty() && s.raw.unpaired.is_empty());
        let mut pairs: Vec<_> = s.raw.pairs.iter().map(|p| (p.begin, p.end)).collect(); pairs.sort_unstable();
        assert_eq!(pairs, expected_pairs[i]);
        for e in &s.raw.events {
            assert_eq!(e.epoch, if id == 1 && e.seq == 4 { 2 } else { 1 });
            assert!(actual.insert((id, e.seq), (e.raw, e.desc, e.kind, e.depth)).is_none());
        }
        let order = &ext.order.as_ref().unwrap().lanes[&id];
        assert!(order.created.is_some() && !order.drains.is_empty() && !order.brackets_at.is_empty());
        assert!(order.tb_decrease.is_none()); assert_eq!(order.final_, Some((order.decoded, 0)));
    }
    let mut expected = BTreeMap::new();
    let truth_bytes = bytes(&d.join("source.truth")); let truth = std::str::from_utf8(&truth_bytes).unwrap();
    for line in truth.lines() {
        let row: Vec<u64> = line.split_whitespace().map(|x| x.parse().unwrap()).collect(); assert_eq!(row.len(), 6);
        let key = (u32::try_from(row[0]).unwrap(), row[1]);
        let value = (row[2], u32::try_from(row[3]).unwrap(), u8::try_from(row[4]).unwrap(), u8::try_from(row[5]).unwrap());
        assert!(expected.insert(key, value).is_none(), "duplicate producer truth identity");
    }
    assert_eq!(expected.len(), 10); assert_eq!(actual, expected, "decoded original events equal producer decisions");
    let meta: serde_json::Value = serde_json::from_slice(&bytes(&d.join("source.json"))).unwrap();
    assert_eq!(meta["schema"], 1); assert_eq!(meta["flags"], 519);
    assert_eq!(meta["slots"][0], meta["slots"][2]); assert_ne!(meta["lane_ids"][0], meta["lane_ids"][2]);
    assert_eq!(meta["lane_ids"], serde_json::json!([1, 2, 3])); assert_eq!(meta["owners"], serde_json::json!([1, 1, 2]));
    assert_eq!(meta["source_bytes"].as_u64(), Some(b.len() as u64));
}

#[test]
#[ignore = "requires independent combined_spu_source artifacts; no encoder dependency"]
fn original_spu_payloads_preserve_real_callback_order_and_owner_chronology() {
    let d = dir(); let b = bytes(&d.join("source.prof")); let _ = full(&b);
    let raw = bytes(&d.join("spu-payloads.bin")); let table = bytes(&d.join("source.chunks"));
    let rows: Vec<_> = std::str::from_utf8(&table).unwrap().lines().map(|s| s.split_whitespace().collect::<Vec<_>>()).collect();
    let mut at = 16; let mut callback = 0usize; let mut rp = 0; let mut declared = BTreeMap::new(); let mut batch = false;
    for (index, row) in rows.iter().enumerate() {
        assert_eq!(row.len(), 5); assert_eq!(row[0].parse::<usize>().unwrap(), index);
        assert!(b.len() - at >= 20); let ty = u32_at(&b, at); let n = u32_at(&b, at + 4) as usize;
        let end = at.checked_add(n + 20).unwrap(); assert!(end <= b.len());
        assert_eq!(u64_at(&b, at + 8), index as u64); assert_eq!(u32_at(&b, end - 4), common::crc32(&b[at..end - 4]));
        assert_eq!(row[1].parse::<u32>().unwrap(), ty); assert_eq!(row[4].parse::<usize>().unwrap(), n);
        let p = &b[at + 16..end - 4];
        if ty == 4 { assert_eq!(u32_at(p, 0), 1); declared.insert(u32_at(p, 4), index); }
        if matches!(ty, 8..=13 | 15 | 16 | 18) {
            assert_eq!(row[2], "spu-drainer"); assert_eq!(row[3].parse::<usize>().unwrap(), callback);
            assert_eq!(u32_at(&raw, rp), ty); assert_eq!(u32_at(&raw, rp + 4) as usize, n);
            assert_eq!(&raw[rp + 8..rp + 8 + n], p, "no SPU metadata/event rewriting"); rp += 8 + n; callback += 1;
            if ty == 8 && u32_at(p, 0) == 2 { batch = true; }
            if ty == 12 {
                let count = u32_at(p, 0); let mut q = 4;
                for _ in 0..count {
                    let owner = u32_at(p, q + 12); assert!(declared.get(&owner).is_some_and(|&i| i < index));
                    let len = u16::from_le_bytes(p[q + 24..q + 26].try_into().unwrap()) as usize; q += 26 + len;
                }
                assert_eq!(q, p.len());
            }
        } else { assert_eq!(row[2], "ppu-test-writer"); assert_eq!(row[3], "-1"); }
        if index == 2 { assert_eq!(ty, 17); }
        if index + 1 == rows.len() { assert_eq!(ty, 6); }
        at = end;
    }
    assert!(batch, "actual drainer must batch two SPU domains"); assert_eq!(at, b.len()); assert_eq!(rp, raw.len());
    let meta: serde_json::Value = serde_json::from_slice(&bytes(&d.join("source.json"))).unwrap();
    assert_eq!(meta["chunks"].as_u64(), Some(rows.len() as u64)); assert_eq!(meta["spu_callbacks"].as_u64(), Some(callback as u64));
}
