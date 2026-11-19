//! Snapshot-support files have their own exact projection equations; ordinary EXPORT is unchanged.
use ps3dk_prof::{hash::crc32, reader};
use ps3dk_prof_viewer::sha256::{digest, Sha256};

fn chunks(bytes: &[u8]) -> Vec<(u32, Vec<u8>)> {
    let mut out = Vec::new();
    let mut p = 16;
    while p < bytes.len() {
        let ty = u32::from_le_bytes(bytes[p..p + 4].try_into().unwrap());
        let n = u32::from_le_bytes(bytes[p + 4..p + 8].try_into().unwrap()) as usize;
        out.push((ty, bytes[p + 16..p + 16 + n].to_vec()));
        p += n + 20;
    }
    out
}
fn encode(header: &[u8], cs: &[(u32, Vec<u8>)]) -> Vec<u8> {
    let mut out = header.to_vec();
    for (seq, (ty, p)) in cs.iter().enumerate() {
        let start = out.len();
        out.extend(ty.to_le_bytes());
        out.extend((p.len() as u32).to_le_bytes());
        out.extend((seq as u64).to_le_bytes());
        out.extend(p);
        out.extend(crc32(&out[start..]).to_le_bytes());
    }
    out
}
fn fixture() -> (Vec<u8>, Vec<(u32, Vec<u8>)>) {
    let source = std::fs::read(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/../ps3dk-prof/tests/fixtures/auto/console-r2.prof"
    ))
    .unwrap();
    let bytes = ps3dk_prof::export::export_bytes(&source, 10..=10).unwrap();
    let cap = reader::read(&bytes).unwrap();
    let mut cs = chunks(&bytes);
    cs.iter_mut().find(|c| c.0 == 40).unwrap().1[2] |= 4;
    let mut hash = Sha256::new();
    hash.update(&bytes[..16]);
    for (ty, p) in &cs {
        hash.update(&ty.to_le_bytes());
        hash.update(&(p.len() as u32).to_le_bytes());
        hash.update(p);
    }
    let mut p = vec![0; 160];
    p[0..2].copy_from_slice(&1u16.to_le_bytes());
    p[2..4].copy_from_slice(&1u16.to_le_bytes());
    p[4..8].copy_from_slice(&1u32.to_le_bytes());
    p[8..16].copy_from_slice(&10u64.to_le_bytes());
    let lane = &cap.lanes[&1];
    p[16..24].copy_from_slice(&lane.frames[0].tb.to_le_bytes());
    p[24..32].copy_from_slice(&lane.frames[1].tb.to_le_bytes());
    p[32..40].copy_from_slice(&(source.len() as u64).to_le_bytes());
    p[40..72].copy_from_slice(&digest(&source));
    p[72..104].copy_from_slice(&hash.finish());
    p[104..108].copy_from_slice(&(cap.lanes.len() as u32).to_le_bytes());
    let clock_bytes: u64 = cs
        .iter()
        .filter(|(t, _)| matches!(t, 2 | 9 | 10 | 18 | 38))
        .map(|(_, p)| p.len() as u64 + 20)
        .sum();
    p[112..120].copy_from_slice(&clock_bytes.to_le_bytes());
    p[120..128]
        .copy_from_slice(&(cap.auto.as_ref().map_or(0, |a| a.frames.len()) as u64).to_le_bytes());
    p[128..160].fill(7); // viewer checks the semantic digest, the structural reader does not.
    for (&inc, lane) in &cap.lanes {
        p.extend(inc.to_le_bytes());
        p.extend(0u32.to_le_bytes());
        p.extend(lane.frames.first().map_or(0, |f| f.id).to_le_bytes());
        p.extend(lane.frames.last().map_or(0, |f| f.id).to_le_bytes());
        p.extend((lane.frames.len() as u64).to_le_bytes());
    }
    let at = cs.iter().position(|c| c.0 == 40).unwrap() + 1;
    cs.insert(at, (49, p));
    (bytes[..16].to_vec(), cs)
}

#[test]
fn valid_snapshot_support_is_readable() {
    let (h, cs) = fixture();
    reader::read(&encode(&h, &cs)).expect("valid chunk49 snapshot support must parse");
}

#[test]
fn snapshot_support_forgery_refused() {
    let (h, cs) = fixture();
    for offset in [0, 2, 4, 16, 32, 120, 104, 112] {
        let mut bad = cs.clone();
        let p = &mut bad.iter_mut().find(|c| c.0 == 49).unwrap().1;
        p[offset] ^= 8;
        assert!(reader::read(&encode(&h, &bad)).is_err(), "offset {offset}");
    }
    let mut bad = cs.clone();
    bad.push(cs.iter().find(|c| c.0 == 49).unwrap().clone());
    assert!(reader::read(&encode(&h, &bad)).is_err());
    let mut bad = cs.clone();
    bad.retain(|c| c.0 != 49);
    assert!(
        reader::read(&encode(&h, &bad)).is_err(),
        "flag without selector"
    );
    let mut bad = cs.clone();
    bad.iter_mut().find(|c| c.0 == 40).unwrap().1[2] &= !4;
    assert!(
        reader::read(&encode(&h, &bad)).is_err(),
        "selector without flag"
    );
    let mut bad = cs.clone();
    bad.iter_mut().find(|c| c.0 == 49).unwrap().1[72] ^= 1;
    assert!(reader::read(&encode(&h, &bad)).is_err(), "support digest");
}

#[test]
fn extreme_lane_bounds_refuse_without_panicking() {
    let (h, mut cs) = fixture();
    let p = &mut cs.iter_mut().find(|c| c.0 == 49).unwrap().1;
    p[168..176].copy_from_slice(&0u64.to_le_bytes());
    p[176..184].copy_from_slice(&u64::MAX.to_le_bytes());
    assert!(reader::read(&encode(&h, &cs)).is_err());
}
