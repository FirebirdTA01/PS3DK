//! Physical/semantic join controls. Original PPU chunks are independently
//! framed; checkpoints at each cut are generated directly, not by advancement.
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof::{rolling, rolling_store};
use ps3dk_prof::rolling_file::{read_ppu, Error};

const PAGE: usize = 4096;
const PREFIX: usize = 12288;
const SLOT: usize = 16384;
const FILE: usize = PREFIX + 3 * SLOT;
const SESSION: [u8; 16] = [0x71; 16];
fn store_limits() -> rolling_store::Limits {
    rolling_store::Limits { max_file_bytes: FILE as u64, max_slot_bytes: SLOT as u32 }
}
fn w32(b: &mut [u8], at: usize, n: u32) { b[at..at + 4].copy_from_slice(&n.to_le_bytes()); }
fn w64(b: &mut [u8], at: usize, n: u64) { b[at..at + 8].copy_from_slice(&n.to_le_bytes()); }
fn checksum(b: &mut [u8], at: usize) { let crc = crc32(&b[..at]); w32(b, at, crc); }
#[derive(Clone)]
struct Segment { checkpoint: Vec<u8>, payload: Vec<u8>, first: u64, next: u64 }
fn source(early_name: &str) -> File {
    let a = vec![begin(0, 0, 0, 1000, 1), bparent(1, 0, 0), end(2, 0, 0, 1010, 1)];
    let b = vec![begin(3, 1, 0, 1100, 2), bparent(4, 0, 0), end(5, 1, 0, 1120, 2)];
    let mut s = scenario(vec![lane(1, [a.clone(), b.clone()].concat())]);
    s.descs = vec![desc(0, early_name, ""), desc(1, "late", "")];
    let mut f = s.file(); let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    f.chunks.extend([chunk(T_RECORDS, records_payload(1, &a)), chunk(T_RECORDS, records_payload(1, &b)), trailer]); f
}
fn offset(f: &File, chunks: usize) -> usize { 16 + f.chunks[..chunks].iter().map(|c| c.payload.len() + 20).sum::<usize>() }
fn checkpoint(f: &File, chunks: usize) -> Vec<u8> {
    rolling::checkpoint_ppu(&f.bytes()[..offset(f, chunks)], rolling::Limits::default()).unwrap()
}
fn payload(f: &File, first: usize, end: usize) -> Vec<u8> { f.bytes()[offset(f, first)..offset(f, end)].to_vec() }
fn segments(f: &File) -> Vec<Segment> {
    [(0, 4), (4, 5), (5, 7)].into_iter().map(|(first, end)| Segment {
        checkpoint: checkpoint(f, first), payload: payload(f, first, end), first: first as u64, next: end as u64,
    }).collect()
}
fn write_root(b: &mut [u8], segments: &[Segment], generation: u64, first: usize, end: usize) {
    let which = generation as usize & 1; let r = &mut b[(which + 1) * PAGE..(which + 2) * PAGE];
    r.fill(0); r[..8].copy_from_slice(b"PFRROOT1"); w64(r, 8, generation); w32(r, 16, (end - first) as u32);
    w64(r, 24, first as u64); w64(r, 32, end as u64);
    let first_chunk = segments.get(first).map_or(0, |s| s.first);
    let next_chunk = end.checked_sub(1).map_or(0, |i| segments[i].next);
    w64(r, 40, first_chunk); w64(r, 48, next_chunk); w64(r, 56, first as u64); w64(r, 64, first_chunk);
    w64(r, 72, segments[..first].iter().map(|s| s.payload.len() as u64).sum()); r[80..96].copy_from_slice(&SESSION);
    for (i, s) in segments[first..end].iter().enumerate() {
        let p = 96 + i * 40; w32(r, p, (first + i) as u32); w32(r, p + 4, s.checkpoint.len() as u32);
        w32(r, p + 8, s.payload.len() as u32); w64(r, p + 16, (first + i) as u64);
        w64(r, p + 24, s.first); w64(r, p + 32, s.next);
    }
    checksum(r, PAGE - 4);
}
fn container(segments: &[Segment]) -> Vec<u8> {
    assert!(segments.is_empty() || segments.len() == 3);
    let mut b = vec![0; FILE]; b[..8].copy_from_slice(b"PS3DKROL"); w32(&mut b, 8, 1); w32(&mut b, 12, PAGE as u32);
    w64(&mut b, 16, FILE as u64); w64(&mut b, 24, FILE as u64); w32(&mut b, 32, 2);
    w32(&mut b, 36, 8192); w32(&mut b, 40, 4096); w32(&mut b, 44, SLOT as u32);
    b[48..64].copy_from_slice(&SESSION); checksum(&mut b[..PAGE], PAGE - 4);
    for (i, s) in segments.iter().enumerate() {
        assert!(s.checkpoint.len() <= 8192 && s.payload.len() <= 4096);
        let p = &mut b[PREFIX + i * SLOT..PREFIX + (i + 1) * SLOT]; let f = SLOT - 64;
        p[..8].copy_from_slice(b"PFRSLOT1"); p[8..24].copy_from_slice(&SESSION);
        w64(p, 24, i as u64); w64(p, 32, i as u64 + 2); w64(p, 40, s.first); w64(p, 48, s.next);
        w32(p, 56, s.checkpoint.len() as u32); w32(p, 60, s.payload.len() as u32);
        let cc = crc32(&s.checkpoint); let pc = crc32(&s.payload); w32(p, 64, cc); w32(p, 68, pc);
        checksum(p, 80); let cp_end = 96 + s.checkpoint.len(); p[96..cp_end].copy_from_slice(&s.checkpoint);
        p[cp_end..cp_end + s.payload.len()].copy_from_slice(&s.payload);
        p[f..f + 8].copy_from_slice(b"PFRCMIT1"); w64(p, f + 8, i as u64); w64(p, f + 16, i as u64 + 2);
        w64(p, f + 24, s.first); w64(p, f + 32, s.next); w32(p, f + 40, cc); w32(p, f + 44, pc);
        let content = crc32(&p[..f]); w32(p, f + 48, content); checksum(&mut p[f..], 60);
    }
    if segments.is_empty() { write_root(&mut b, segments, 1, 0, 0); }
    else { write_root(&mut b, segments, 3, 0, 2); write_root(&mut b, segments, 4, 1, 3); }
    b
}

#[test]
fn analyzes_all_retained_segments_and_accepts_trailer_only_in_last() {
    let f = source("early"); assert!(reader::read(&f.bytes()).unwrap().incomplete.is_empty());
    let b = container(&segments(&f)); let got = read_ppu(&b, store_limits(), rolling::Limits::default())
        .expect("physical PPU join is unavailable");
    assert_eq!(got.store_view.generation, 4); assert!(!got.store_view.recovered);
    assert_eq!((got.retained.first_chunk, got.retained.next_chunk), (4, 7));
    assert_eq!(got.retained.prefix_bytes, (offset(&f, 4) - 16) as u64);
    assert_eq!(got.retained.prefix_bytes, got.store_view.evicted_payload_bytes);
    assert!(got.retained.incomplete.is_empty());
    let lane = &got.retained.lanes[&1]; assert_eq!((lane.prefix_written, lane.retained_written), (0, 6));
    for (name, ticks) in [("early", 10), ("late", 20)] {
        let z = got.retained.zones.iter().find(|z| z.name == name).expect("every retained segment contributes");
        assert_eq!((z.completed, z.left_censored, z.total_ticks), (1, 0, Some(ticks)));
    }
}
#[test]
fn internally_valid_intermediate_checkpoint_forgery_with_all_outer_crcs_repaired_refuses() {
    let f = source("early"); let mut altered = f.clone();
    // Different completed prefix timestamp, same original chunk/byte frontier.
    // This produces valid typed state that cannot follow the stored first segment.
    altered.chunks[4].payload[8 + 2 * 32 + 16..8 + 2 * 32 + 24].copy_from_slice(&1020u64.to_le_bytes());
    let mut parts = segments(&f); parts[2].checkpoint = checkpoint(&altered, 5);
    assert!(rolling::read_ppu_suffix(&parts[2].checkpoint, &parts[2].payload, rolling::Limits::default()).is_ok());
    assert_ne!(rolling::advance_ppu(&parts[1].checkpoint, &parts[1].payload, rolling::Limits::default()).unwrap(), parts[2].checkpoint);
    let b = container(&parts); assert_eq!(rolling_store::read(&b, store_limits()).unwrap().view.generation, 4);
    assert!(matches!(read_ppu(&b, store_limits(), rolling::Limits::default()), Err(Error::Transition { segment: 2 })));
}
#[test]
fn first_checkpoint_chunk_frontier_must_match_physical_window() {
    let f = source("early"); let mut parts = segments(&f); parts[1].checkpoint = checkpoint(&f, 5);
    let b = container(&parts); assert!(rolling_store::read(&b, store_limits()).is_ok());
    assert!(read_ppu(&b, store_limits(), rolling::Limits::default()).is_err());
}
#[test]
fn first_checkpoint_byte_frontier_must_equal_physically_evicted_payload_bytes() {
    let f = source("early"); let alternate = source("longer early descriptor"); let mut parts = segments(&f);
    parts[1].checkpoint = checkpoint(&alternate, 4); parts[2].checkpoint = checkpoint(&alternate, 5);
    assert_eq!(rolling::advance_ppu(&parts[1].checkpoint, &parts[1].payload, rolling::Limits::default()).unwrap(), parts[2].checkpoint);
    let b = container(&parts); assert!(rolling_store::read(&b, store_limits()).is_ok());
    assert!(matches!(read_ppu(&b, store_limits(), rolling::Limits::default()), Err(Error::Boundary(_))));
}
#[test]
fn terminal_trailer_cannot_have_a_later_even_empty_segment() {
    let f = source("early"); let mut parts = segments(&f);
    parts[1].payload = payload(&f, 4, 7); parts[1].next = 7;
    parts[2] = Segment { checkpoint: checkpoint(&f, 6), payload: vec![], first: 7, next: 7 };
    let b = container(&parts); assert!(rolling_store::read(&b, store_limits()).is_ok());
    assert!(rolling::read_ppu_suffix(&parts[1].checkpoint, &parts[1].payload, rolling::Limits::default()).is_ok());
    assert!(matches!(read_ppu(&b, store_limits(), rolling::Limits::default()), Err(Error::Semantic(rolling::Error::Checkpoint(_)))));
}
#[test]
fn metadata_only_segment_preserves_checkpoint_identity() {
    let f = source("early"); let mut parts = segments(&f);
    parts[1].payload.clear(); parts[1].next = 4;
    parts[2] = Segment { checkpoint: checkpoint(&f, 4), payload: payload(&f, 4, 7), first: 4, next: 7 };
    let got = read_ppu(&container(&parts), store_limits(), rolling::Limits::default()).unwrap();
    assert_eq!(got.retained.lanes[&1].retained_written, 6); assert_eq!(got.retained.zones.len(), 2);
}
#[test]
fn physical_recovery_reports_and_analyzes_only_the_selected_complete_chain() {
    let f = source("early"); let mut b = container(&segments(&f)); b[PREFIX + 2 * SLOT] ^= 1;
    let got = read_ppu(&b, store_limits(), rolling::Limits::default()).unwrap();
    assert_eq!(got.store_view.generation, 3); assert!(got.store_view.recovered);
    assert_eq!((got.retained.first_chunk, got.retained.next_chunk, got.retained.prefix_bytes), (0, 5, 0));
    assert_eq!(got.retained.lanes[&1].retained_written, 3);
    assert!(got.retained.zones.iter().any(|z| z.name == "early"));
    assert!(!got.retained.zones.iter().any(|z| z.name == "late"));
    assert!(!got.retained.incomplete.is_empty());
}
#[test]
fn budgets_apply_to_physical_input_and_total_retained_record_work() {
    let b = container(&segments(&source("early")));
    let small = rolling_store::Limits { max_file_bytes: FILE as u64 - 1, ..store_limits() };
    assert!(matches!(read_ppu(&b, small, rolling::Limits::default()), Err(Error::Store(rolling_store::Error::Budget(_)))));
    // Each segment has 3 rows, but all 6 retained rows must consume the budget.
    let small = rolling::Limits { work_records: 3, ..rolling::Limits::default() };
    assert!(matches!(read_ppu(&b, store_limits(), small), Err(Error::Semantic(rolling::Error::Limit("record work")))));
}
#[test]
fn unsupported_record_family_is_refused_after_physical_validation() {
    let mut f = source("early"); f.chunks[5] = chunk(40, vec![]);
    let b = container(&segments(&f)); assert!(rolling_store::read(&b, store_limits()).is_ok());
    assert!(matches!(read_ppu(&b, store_limits(), rolling::Limits::default()),
        Err(Error::Semantic(rolling::Error::Unsupported { chunk: Some(40), .. }))));
}
#[test]
fn empty_physical_root_has_no_semantic_checkpoint() {
    let b = container(&[]); assert!(rolling_store::read(&b, store_limits()).is_ok());
    assert!(matches!(read_ppu(&b, store_limits(), rolling::Limits::default()), Err(Error::EmptyStore)));
}
