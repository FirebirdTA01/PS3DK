//! Independent physical-container fixtures. No semantic checkpoint claims.
#[path = "../src/rolling_store.rs"]
mod rolling_store;
use rolling_store::{read, Error, Limits};

const PAGE: usize = 4096;
const PREFIX: usize = 12288;
const FILE: usize = PREFIX + 3 * PAGE;
const SESSION: [u8; 16] = [0x42; 16];
fn limits() -> Limits { Limits { max_file_bytes: FILE as u64, max_slot_bytes: PAGE as u32 } }
fn w32(b: &mut [u8], p: usize, n: u32) { b[p..p + 4].copy_from_slice(&n.to_le_bytes()); }
fn w64(b: &mut [u8], p: usize, n: u64) { b[p..p + 8].copy_from_slice(&n.to_le_bytes()); }
fn u32_at(b: &[u8], p: usize) -> u32 { u32::from_le_bytes(b[p..p + 4].try_into().unwrap()) }
fn crc(b: &[u8]) -> u32 {
    let mut c = !0u32;
    for &v in b { c ^= u32::from(v); for _ in 0..8 { c = (c >> 1) ^ (0xedb88320 & 0u32.wrapping_sub(c & 1)); } }
    !c
}
fn checksum(b: &mut [u8], at: usize) { let c = crc(&b[..at]); w32(b, at, c); }
fn root(b: &mut [u8], generation: u64, first: u64, end: u64, chunks: bool) {
    let which = generation as usize & 1;
    let b = &mut b[(which + 1) * PAGE..(which + 2) * PAGE];
    b.fill(0); b[..8].copy_from_slice(b"PFRROOT1");
    w64(b, 8, generation); w32(b, 16, (end - first) as u32);
    w64(b, 24, first); w64(b, 32, end);
    w64(b, 40, if chunks { first } else { 0 }); w64(b, 48, if chunks { end } else { 0 });
    w64(b, 56, first); w64(b, 64, if chunks { first } else { 0 });
    w64(b, 72, if chunks { first * 24 } else { 0 }); b[80..96].copy_from_slice(&SESSION);
    for (i, seq) in (first..end).enumerate() {
        let p = 96 + i * 40;
        w32(b, p, (seq % 3) as u32); w32(b, p + 4, 8); w32(b, p + 8, if chunks { 24 } else { 0 });
        w64(b, p + 16, seq); w64(b, p + 24, if chunks { seq } else { 0 });
        w64(b, p + 32, if chunks { seq + 1 } else { 0 });
    }
    checksum(b, PAGE - 4);
}
fn slot(b: &mut [u8], seq: u64, chunks: bool) {
    let at = PREFIX + seq as usize % 3 * PAGE;
    let b = &mut b[at..at + PAGE]; b.fill(0);
    b[..8].copy_from_slice(b"PFRSLOT1"); b[8..24].copy_from_slice(&SESSION);
    w64(b, 24, seq); w64(b, 32, seq + 2);
    w64(b, 40, if chunks { seq } else { 0 }); w64(b, 48, if chunks { seq + 1 } else { 0 });
    w32(b, 56, 8); w32(b, 60, if chunks { 24 } else { 0 }); w64(b, 96, seq);
    if chunks {
        w32(b, 104, 43); w32(b, 108, 4); w64(b, 112, seq); w32(b, 120, seq as u32 ^ 0xaabbccdd);
        checksum(&mut b[104..128], 20);
    }
    let f = PAGE - 64;
    b[f..f + 8].copy_from_slice(b"PFRCMIT1");
    for (dst, src) in [(8, 24), (16, 32), (24, 40), (32, 48)] {
        let n = u64::from_le_bytes(b[src..src + 8].try_into().unwrap()); w64(b, f + dst, n);
    }
    repair_slot_checksums(b);
}
fn repair_slot_checksums(b: &mut [u8]) {
    let cp = u32_at(b, 56) as usize; let payload = u32_at(b, 60) as usize;
    let cc = crc(&b[96..96 + cp]); let pc = crc(&b[96 + cp..96 + cp + payload]);
    w32(b, 64, cc); w32(b, 68, pc); checksum(b, 80);
    let f = PAGE - 64; w32(b, f + 40, cc); w32(b, f + 44, pc);
    let content = crc(&b[..f]); w32(b, f + 48, content); checksum(&mut b[f..], 60);
}
fn config() -> Vec<u8> {
    let mut b = vec![0; FILE]; b[..8].copy_from_slice(b"PS3DKROL");
    w32(&mut b, 8, 1); w32(&mut b, 12, PAGE as u32); w64(&mut b, 16, FILE as u64);
    w64(&mut b, 24, FILE as u64); w32(&mut b, 32, 2); w32(&mut b, 36, 16);
    w32(&mut b, 40, 64); w32(&mut b, 44, PAGE as u32); b[48..64].copy_from_slice(&SESSION);
    checksum(&mut b[..PAGE], PAGE - 4); b
}
fn fixture() -> Vec<u8> {
    let mut b = config(); for seq in 0..3 { slot(&mut b, seq, true); }
    root(&mut b, 3, 0, 2, true); root(&mut b, 4, 1, 3, true); b
}
fn no_chain(b: &[u8]) { assert_eq!(read(b, limits()).unwrap_err(), Error::NoCommittedRoot); }

#[test]
fn newest_complete_chain_borrows_bytes_and_preserves_original_frontiers() {
    let b = fixture(); let got = read(&b, limits()).expect("physical rolling reader is unavailable");
    assert_eq!((got.view.generation, got.view.first_segment, got.view.next_segment), (4, 1, 3));
    assert_eq!((got.view.first_chunk, got.view.next_chunk), (1, 3));
    assert_eq!((got.view.evicted_segments, got.view.evicted_chunks, got.view.evicted_payload_bytes), (1, 1, 24));
    assert_eq!(got.view.retained_segments, 2); assert!(!got.view.recovered);
    assert_eq!((got.config.file_bytes, got.config.max_bytes, got.config.retained_slots), (FILE as u64, FILE as u64, 2));
    assert_eq!((got.config.checkpoint_bytes, got.config.payload_bytes, got.config.slot_bytes), (16, 64, PAGE as u32));
    assert_eq!(got.config.session, SESSION);
    assert_eq!(got.segments.len(), 2);
    for (s, seq) in got.segments.iter().zip(1..3u64) {
        assert_eq!((s.segment, s.first_chunk, s.next_chunk, s.physical_slot), (seq, seq, seq + 1, seq as u32));
        assert_eq!(s.checkpoint, seq.to_le_bytes()); assert_eq!(s.payload.len(), 24);
        assert_eq!(s.checkpoint.as_ptr(), b[PREFIX + seq as usize * PAGE + 96..].as_ptr());
    }
}
#[test]
fn torn_new_root_and_corrupt_newest_unique_slot_fall_back_without_stitching() {
    for at in [PAGE + 25, PREFIX + 2 * PAGE + 96] {
        let mut b = fixture(); b[at] ^= 1; let got = read(&b, limits()).unwrap();
        assert_eq!((got.view.generation, got.view.first_chunk, got.view.next_chunk), (3, 0, 2));
        assert!(got.view.recovered); assert_eq!(got.segments[0].segment, 0);
    }
}
#[test]
fn damaged_alternate_is_reported_even_when_newest_is_valid() {
    let mut b = fixture(); b[2 * PAGE] ^= 1;
    let got = read(&b, limits()).unwrap(); assert_eq!(got.view.generation, 4); assert!(got.view.recovered);
    b[2 * PAGE..3 * PAGE].fill(0);
    let got = read(&b, limits()).unwrap(); assert_eq!(got.view.generation, 4); assert!(got.view.recovered);
}
#[test]
fn initial_empty_and_metadata_only_segments_are_valid() {
    let mut b = config(); root(&mut b, 1, 0, 0, false);
    let got = read(&b, limits()).unwrap(); assert_eq!(got.view.generation, 1);
    assert!(!got.view.recovered); assert!(got.segments.is_empty());
    slot(&mut b, 0, false); root(&mut b, 2, 0, 1, false);
    let got = read(&b, limits()).unwrap(); assert_eq!(got.view.generation, 2);
    assert!(!got.view.recovered); assert_eq!(got.view.next_chunk, 0); assert!(got.segments[0].payload.is_empty());
}
#[test]
fn never_resurrect_slots_without_a_complete_committed_chain() {
    let mut b = fixture(); b[PAGE..PREFIX].fill(0); no_chain(&b);
    let mut b = fixture(); b[PREFIX + PAGE] ^= 1; no_chain(&b);
}
#[test]
fn configuration_length_cap_reserved_and_arithmetic_are_checked() {
    for (at, value, width) in [(8, 2, 4), (12, 2048, 4), (16, FILE as u64 + 1, 8),
        (24, FILE as u64 - 1, 8), (32, 1, 4), (32, 65, 4), (40, 19, 4),
        (44, 8192, 4), (36, u32::MAX as u64, 4), (64, 1, 4)] {
        let mut b = fixture(); if width == 8 { w64(&mut b, at, value); } else { w32(&mut b, at, value as u32); }
        checksum(&mut b[..PAGE], PAGE - 4); assert!(read(&b, limits()).is_err(), "config offset {at}");
    }
    let mut b = fixture(); b[48] ^= 1; assert!(read(&b, limits()).is_err());
    for n in [0, 8, PAGE - 1, PREFIX - 1, FILE - 1] { assert!(read(&fixture()[..n], limits()).is_err()); }
    let mut b = fixture(); b.push(0); assert!(read(&b, Limits { max_file_bytes: FILE as u64 + 1, ..limits() }).is_err());
}
#[test]
fn caller_budgets_refuse_before_container_work() {
    let b = fixture();
    assert!(matches!(read(&b, Limits { max_file_bytes: FILE as u64 - 1, ..limits() }), Err(Error::Budget(_))));
    assert!(matches!(read(&b, Limits { max_slot_bytes: PAGE as u32 - 1, ..limits() }), Err(Error::Budget(_))));
    assert!(read(&b, limits()).is_ok());
    let mut huge = b.clone(); w64(&mut huge, 16, u64::MAX); checksum(&mut huge[..PAGE], PAGE - 4);
    assert!(matches!(read(&huge, limits()), Err(Error::Budget(_))));
}
#[test]
fn valid_root_crc_does_not_hide_invalid_identity_ranges_or_entry_limits() {
    for (at, value, width) in [(8, 0, 8), (16, 3, 4), (20, 1, 4), (24, 9, 8),
        (32, u64::MAX, 8), (40, 99, 8), (56, 99, 8), (64, 99, 8), (72, 9999, 8),
        (80, 0, 4), (96, 3, 4), (100, 17, 4), (104, 65, 4), (108, 1, 4),
        (112, 99, 8), (120, 99, 8), (128, 99, 8), (176, 1, 4)] {
        let mut b = fixture();
        for which in 0..2 {
            let r = &mut b[(which + 1) * PAGE..(which + 2) * PAGE];
            if width == 8 { w64(r, at, value); } else { w32(r, at, value as u32); }
            checksum(r, PAGE - 4);
        }
        no_chain(&b);
    }
    let mut b = fixture();
    for which in 0..2 {
        let r = &mut b[(which + 1) * PAGE..(which + 2) * PAGE]; let physical = u32_at(r, 96);
        w32(r, 136, physical); checksum(r, PAGE - 4);
    }
    no_chain(&b);
}
#[test]
fn shared_slot_identity_lengths_crc_padding_and_footer_are_all_required() {
    for at in [0, 8, 24, 32, 40, 48, 56, 60, 64, 68, 72, 80, 84, 96, 104, 128,
        PAGE - 64, PAGE - 56, PAGE - 48, PAGE - 40, PAGE - 32, PAGE - 24, PAGE - 20,
        PAGE - 16, PAGE - 12, PAGE - 4] {
        let mut b = fixture(); b[PREFIX + PAGE + at] ^= 1; no_chain(&b);
    }
    // Recompute outer checksums to require the identity/length/reserved rules,
    // not merely a generic CRC rejection of every malformed slot.
    for at in [0, 8, 24, 32, 40, 48, 56, 60, 72, 84, 128,
        PAGE - 64, PAGE - 56, PAGE - 48, PAGE - 40, PAGE - 32, PAGE - 12] {
        let mut b = fixture(); let s = &mut b[PREFIX + PAGE..PREFIX + 2 * PAGE];
        s[at] ^= 1; repair_slot_checksums(s); no_chain(&b);
    }
}
#[test]
fn whole_chunk_sequence_length_and_crc_validate_even_with_repaired_slot_checksums() {
    for (at, value, width) in [(108, 5, 4), (108, 1_048_577, 4), (112, 99, 8),
        (112, u64::MAX, 8), (124, 0, 4)] {
        let mut b = fixture(); let s = &mut b[PREFIX + PAGE..PREFIX + 2 * PAGE];
        if width == 8 { w64(s, at, value); } else { w32(s, at, value as u32); }
        repair_slot_checksums(s); no_chain(&b);
    }
    let mut b = fixture(); let s = &mut b[PREFIX + PAGE..PREFIX + 2 * PAGE];
    w64(s, 112, 99); checksum(&mut s[104..128], 20); repair_slot_checksums(s); no_chain(&b);
}

#[test]
#[ignore = "requires the artifact generated by tests/host/ps3dk-prof-rolling/run.py"]
fn c_produced_rotations_match_committed_view_and_borrowed_payload() {
    let path = std::env::var_os("PS3DK_PROF_ROLLING_FIXTURE").expect("set PS3DK_PROF_ROLLING_FIXTURE to run.py's rotated.profr");
    let b = std::fs::read(path).expect("read C-produced rotated.profr");
    let got = read(&b, Limits { max_file_bytes: 28672, max_slot_bytes: 4096 }).unwrap();
    assert_eq!((got.config.file_bytes, got.view.generation, got.view.first_chunk, got.view.next_chunk), (28672, 13, 9, 12));
    assert_eq!((got.view.evicted_segments, got.view.evicted_chunks, got.view.evicted_payload_bytes), (9, 9, 216));
    assert_eq!(got.segments.len(), 3);
    for (s, seq) in got.segments.iter().zip(9..12u64) {
        assert_eq!((s.segment, s.first_chunk, s.next_chunk), (seq, seq, seq + 1));
        assert_eq!(s.checkpoint, seq.to_le_bytes());
        assert_eq!(u64::from_le_bytes(s.payload[8..16].try_into().unwrap()), seq);
    }
}
