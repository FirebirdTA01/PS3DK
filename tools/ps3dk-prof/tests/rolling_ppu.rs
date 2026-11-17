//! Offline semantic restart tests. No physical ring/runtime/live feature claim.
//! Test writer is independent of the production reader/codec.
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof::{Incomplete, Rule};
use ps3dk_prof::model::{CHUNK_WINDOW_BEGIN, CHUNK_WINDOW_END, FLAG_WINDOWS};

use ps3dk_prof::rolling;
use rolling::{Error, Limits};

fn win_begin() -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u32(0).u64(0).u8(1).u8(1).u16(0).u32(0).u32(0);
    chunk(CHUNK_WINDOW_BEGIN, b.0)
}
fn win_end(id: u32, last: u64) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(id).u8(1).u8(0).u16(0).u64(last);
    b.0.extend_from_slice(&[0; 12]);
    chunk(CHUNK_WINDOW_END, b.0)
}
fn fixture() -> File {
    let pre = vec![frame(0, 1000, 0), begin(1, 0, 0, 1010, 1), bparent(2, 0, 0)];
    let tail = vec![begin(3, 1, 1, 1020, 2), bparent(4, 1, 1), end(5, 1, 1, 1040, 2),
        end(6, 0, 0, 1050, 1), frame(7, 1100, 1), begin(8, 2, 0, 1110, 3), bparent(9, 0, 0),
        end(10, 2, 0, 1140, 3), frame(11, 1200, 2)];
    let mut all = pre.clone(); all.extend_from_slice(&tail);
    let mut source = scenario(vec![lane(1, all)]);
    source.descs = vec![desc(0, "outer", ""), desc(1, "inner", ""), desc(2, "survivor", "")];
    let mut f = source.file();
    let trailer = f.chunks.pop().unwrap();
    f.chunks.truncate(4);
    f.chunks.extend([win_begin(), chunk(T_RECORDS, records_payload(1, &pre)),
        chunk(T_RECORDS, records_payload(1, &tail)), win_end(1, 2), trailer]);
    f.flags = FLAG_WINDOWS;
    f
}
fn cut(f: &File, count: usize) -> usize {
    16 + f.chunks[..count].iter().map(|c| c.payload.len() + 20).sum::<usize>()
}
fn checkpoint(f: &File, count: usize) -> (Vec<u8>, Vec<u8>) {
    let bytes = f.bytes(); let at = cut(f, count);
    let cp = rolling::checkpoint_ppu(&bytes[..at], Limits::default()).expect("semantic checkpoint implemented");
    (cp, bytes[at..].to_vec())
}
fn load_suffix(f: &File, count: usize) -> rolling::RetainedPpu {
    let (cp, tail) = checkpoint(f, count);
    rolling::read_ppu_suffix(&cp, &tail, Limits::default()).expect("valid retained PPU suffix")
}
fn expect_rule(result: Result<rolling::RetainedPpu, Error>, want: Rule) {
    match result {
        Err(Error::Malformed(m)) => assert_eq!(m.rule, want, "{}", m.detail),
        other => panic!("expected {want:?}, got {other:?}"),
    }
}

#[test]
fn closed_prefix_zones_do_not_replay_as_retained_records() {
    // Break caught: storing/replaying the discarded event prefix to fake v1.
    let mut small = scenario(vec![lane(1, vec![begin(0, 0, 0, 1000, 1), bparent(1, 0, 0), end(2, 0, 0, 1010, 1)])]).file();
    small.chunks.pop();
    let mut many = Vec::new();
    for id in 1..=512u64 {
        let seq = (id - 1) * 3;
        many.extend([begin(seq, 0, 0, 1000 + id * 20, id), bparent(seq + 1, 0, 0),
            end(seq + 2, 0, 0, 1010 + id * 20, id)]);
    }
    let mut large = scenario(vec![lane(1, many)]).file(); large.chunks.pop();
    let a = rolling::checkpoint_ppu(&small.bytes(), Limits::default()).expect("checkpoint for one closed zone");
    let b = rolling::checkpoint_ppu(&large.bytes(), Limits::default()).expect("checkpoint for many closed zones");
    assert_eq!(a.len(), b.len(), "closed records and span-ID sets must compact to counters/high-water state");
    assert!(b.len() < 8192);
}

#[test]
fn retained_suffix_keeps_exact_prefix_counts_and_censors_only_crossing_zone() {
    let f = fixture();
    assert!(reader::read(&f.bytes()).unwrap().incomplete.is_empty(), "independent full fixture is valid");
    let got = load_suffix(&f, 6);
    assert!(got.incomplete.is_empty());
    assert_eq!((got.first_chunk, got.next_chunk), (6, 9));
    assert_eq!(got.prefix_bytes, (cut(&f, 6) - 16) as u64);
    let lane = &got.lanes[&1];
    assert_eq!((lane.prefix_written, lane.retained_written), (3, 9));
    assert_eq!((lane.prefix_gap_sum, lane.retained_gap_sum), (0, 0));
    let final_ = lane.final_entry.as_ref().unwrap();
    assert_eq!((final_.produced_count, final_.written_count, final_.dropped_count), (12, 12, 0));
    let outer = got.zones.iter().find(|z| z.name == "outer").unwrap();
    assert_eq!((outer.completed, outer.left_censored, outer.open, outer.total_ticks), (0, 1, 0, None));
    let inner = got.zones.iter().find(|z| z.name == "inner").unwrap();
    assert_eq!((inner.completed, inner.left_censored, inner.total_ticks), (1, 0, Some(20)));
    let survivor = got.zones.iter().find(|z| z.name == "survivor").unwrap();
    assert_eq!((survivor.completed, survivor.left_censored, survivor.total_ticks), (1, 0, Some(30)));
    assert_eq!(got.windows.len(), 1);
    assert!(got.windows[0].left_clipped);
    assert_eq!((got.windows[0].original.first_frame, got.windows[0].original.last_frame), (0, Some(2)));
}

#[test]
fn typed_bootstrap_state_supports_cuts_before_header_or_clocks_without_synthetic_chunks() {
    let f = fixture();
    for chunks in [0, 1, 2, 4] {
        let got = load_suffix(&f, chunks);
        assert!(got.incomplete.is_empty());
        assert_eq!(got.first_chunk, chunks as u64);
        assert_eq!(got.lanes[&1].prefix_written, 0);
        assert_eq!(got.lanes[&1].retained_written, 12);
        assert_eq!(got.zones.iter().find(|z| z.name == "outer").unwrap().total_ticks, Some(40));
    }
}

#[test]
fn source_final_counts_are_checked_against_prefix_plus_retained() {
    let original = fixture(); let (cp, _) = checkpoint(&original, 6);
    let mut bad = original.clone();
    bad.chunks.last_mut().unwrap().payload[24..32].copy_from_slice(&11u64.to_le_bytes());
    expect_rule(rolling::read_ppu_suffix(&cp, &bad.bytes()[cut(&bad, 6)..], Limits::default()), Rule::WrittenCountMismatch);
    let mut bad = original;
    bad.chunks.last_mut().unwrap().payload[16..24].copy_from_slice(&13u64.to_le_bytes());
    expect_rule(rolling::read_ppu_suffix(&cp, &bad.bytes()[cut(&bad, 6)..], Limits::default()), Rule::ProducedCountArithmetic);
}

#[test]
fn imported_stack_and_window_identity_remain_strict() {
    let original = fixture(); let (cp, _) = checkpoint(&original, 6);
    let mut bad = original.clone();
    // Fourth retained record ends outer span: changing its identity must fail.
    let at = 8 + 3 * 32 + 24;
    bad.chunks[6].payload[at..at + 8].copy_from_slice(&99u64.to_le_bytes());
    expect_rule(rolling::read_ppu_suffix(&cp, &bad.bytes()[cut(&bad, 6)..], Limits::default()), Rule::StackEndMismatch);
    let mut bad = original;
    bad.chunks[7] = win_end(2, 2);
    expect_rule(rolling::read_ppu_suffix(&cp, &bad.bytes()[cut(&bad, 6)..], Limits::default()), Rule::Windows);
}

#[test]
fn original_chunk_sequences_and_crc_remain_strict() {
    let original = fixture(); let (cp, _) = checkpoint(&original, 6);
    let mut bad = original.clone(); bad.seq_override = Some((6, 0));
    expect_rule(rolling::read_ppu_suffix(&cp, &bad.bytes()[cut(&bad, 6)..], Limits::default()), Rule::ChunkSeqOrder);
    let mut bad = original; bad.bad_crc = Some(6);
    expect_rule(rolling::read_ppu_suffix(&cp, &bad.bytes()[cut(&bad, 6)..], Limits::default()), Rule::ChunkCrc);
}

#[test]
fn real_prefix_loss_is_preserved_and_not_replaced_by_eviction_loss() {
    let pre = vec![frame(0, 1000, 0), gap(1, 2), frame(3, 1010, 1)];
    let tail = vec![begin(4, 0, 0, 1020, 1), bparent(5, 0, 0), end(6, 0, 0, 1040, 1), frame(7, 1050, 2)];
    let mut all = pre.clone(); all.extend_from_slice(&tail);
    let mut f = scenario(vec![lane(1, all)]).file();
    f.chunks[4] = chunk(T_RECORDS, records_payload(1, &pre));
    f.chunks.insert(5, chunk(T_RECORDS, records_payload(1, &tail)));
    assert!(reader::read(&f.bytes()).unwrap().incomplete.is_empty());
    let got = load_suffix(&f, 5); let l = &got.lanes[&1];
    assert_eq!((l.prefix_written, l.retained_written, l.prefix_gap_sum, l.retained_gap_sum), (2, 4, 2, 0));
    assert_eq!(l.final_entry.as_ref().unwrap().dropped_count, 2);
    assert!(got.zones.iter().all(|z| z.total_ticks.is_none()), "real producer loss still invalidates lane measurement");
}

#[test]
fn retired_prefix_incarnation_keeps_its_final_counts_and_frees_reused_slot() {
    let first = vec![begin(0, 0, 0, 1000, 1), bparent(1, 0, 0), end(2, 0, 0, 1010, 1)];
    let second = vec![begin(0, 0, 0, 1020, 1), bparent(1, 0, 0), end(2, 0, 0, 1040, 1)];
    let mut old = lane(1, first.clone()); old.state = 1;
    let source = scenario(vec![old.clone(), lane(2, second.clone())]);
    let mut f = source.file();
    let trailer = f.chunks.pop().unwrap();
    f.chunks.truncate(3);
    let mut a = thr(1); a.slot = 1;
    let mut b = thr(2); b.slot = 1;
    f.chunks.extend([chunk(T_THREADS, threads_payload(&[a])), chunk(T_RECORDS, records_payload(1, &first)),
        chunk(T_RETIRED, retired_payload(&[old.fin()])), chunk(T_THREADS, threads_payload(&[b])),
        chunk(T_RECORDS, records_payload(2, &second)), trailer]);
    assert!(reader::read(&f.bytes()).unwrap().incomplete.is_empty());
    let got = load_suffix(&f, 6);
    assert!(got.incomplete.is_empty());
    assert_eq!((got.lanes[&1].prefix_written, got.lanes[&1].retained_written), (3, 0));
    assert_eq!(got.lanes[&1].final_entry.as_ref().unwrap().written_count, 3);
    assert_eq!((got.lanes[&2].prefix_written, got.lanes[&2].retained_written), (0, 3));
    assert_eq!(got.zones.iter().map(|z| z.completed).sum::<u64>(), 1);
}

#[test]
fn torn_tail_remains_explicitly_incomplete_and_checkpoint_creation_requires_whole_prefix() {
    let f = fixture(); let (cp, mut tail) = checkpoint(&f, 6);
    tail.truncate(tail.len() - 3);
    let got = rolling::read_ppu_suffix(&cp, &tail, Limits::default()).unwrap();
    assert!(got.incomplete.contains(&Incomplete::TornTail));
    assert!(got.incomplete.contains(&Incomplete::NoTrailer));
    let bytes = f.bytes();
    assert!(rolling::checkpoint_ppu(&bytes[..cut(&f, 6) - 1], Limits::default()).is_err());
    assert!(rolling::checkpoint_ppu(&bytes, Limits::default()).is_err(), "cannot checkpoint after a final trailer");
}

#[test]
fn unsupported_record_families_are_rejected_before_any_suffix_claim() {
    for flag in [1, 8, 16, 32, 64 | 1, 128 | 1, 256] {
        let mut f = fixture(); f.flags |= flag;
        assert!(matches!(rolling::checkpoint_ppu(&f.bytes()[..cut(&f, 6)], Limits::default()),
            Err(Error::Unsupported { .. })));
    }
    let f = fixture(); let (cp, _) = checkpoint(&f, 6);
    let mut bad = f; bad.chunks.insert(6, chunk(40, vec![0; 112]));
    assert!(matches!(rolling::read_ppu_suffix(&cp, &bad.bytes()[cut(&bad, 6)..], Limits::default()),
        Err(Error::Unsupported { chunk: Some(40), .. })));
}

fn repair_checkpoint_crc(bytes: &mut [u8]) {
    let end = bytes.len() - 4;
    let crc = crc32(&bytes[..end]); bytes[end..].copy_from_slice(&crc.to_le_bytes());
}
fn section(bytes: &[u8], kind: u16) -> usize {
    let mut at = 64;
    while at + 16 <= bytes.len() - 4 {
        if u16::from_le_bytes(bytes[at..at + 2].try_into().unwrap()) == kind { return at; }
        let len = u32::from_le_bytes(bytes[at + 8..at + 12].try_into().unwrap()) as usize;
        at += 16 + len;
    }
    panic!("required checkpoint section {kind} absent");
}

#[test]
fn bounded_schema_rejects_mutations_even_when_checkpoint_crc_is_repaired() {
    let f = fixture(); let (cp, tail) = checkpoint(&f, 6);
    assert_eq!(&cp[..8], b"PFRCKP01");
    let mut schema = cp.clone(); schema[8..10].copy_from_slice(&2u16.to_le_bytes()); repair_checkpoint_crc(&mut schema);
    assert!(matches!(rolling::read_ppu_suffix(&schema, &tail, Limits::default()), Err(Error::Checkpoint(_))));
    let mut unknown = cp.clone(); unknown[64..66].copy_from_slice(&65535u16.to_le_bytes()); repair_checkpoint_crc(&mut unknown);
    assert!(matches!(rolling::read_ppu_suffix(&unknown, &tail, Limits::default()), Err(Error::Checkpoint(_))));
    let mut counts = cp.clone(); let row = section(&counts, 2) + 16;
    // PPU row: u32 row_bytes, u32 incarnation, u64 prefix_written.
    counts[row + 8..row + 16].copy_from_slice(&4u64.to_le_bytes()); repair_checkpoint_crc(&mut counts);
    assert!(matches!(rolling::read_ppu_suffix(&counts, &tail, Limits::default()), Err(Error::Checkpoint(_))),
        "prefix written + GAP sum must equal next source sequence, not merely fit final totals");
    let mut huge = cp.clone(); let at = section(&huge, 2);
    huge[at + 4..at + 8].copy_from_slice(&u32::MAX.to_le_bytes()); repair_checkpoint_crc(&mut huge);
    assert!(matches!(rolling::read_ppu_suffix(&huge, &tail, Limits::default()), Err(Error::Limit(_)) | Err(Error::Checkpoint(_))));
}

#[test]
fn prefix_counter_claims_must_fit_exact_original_record_chunk_bytes() {
    let f = fixture(); let (mut cp, _) = checkpoint(&f, 6);
    let row = section(&cp, 2) + 16;
    // Keep both redundant arithmetic equations consistent, but claim one more
    // sequenced prefix record than the actual original chunk bytes can contain.
    cp[row + 8..row + 16].copy_from_slice(&4u64.to_le_bytes());
    cp[row + 40..row + 48].copy_from_slice(&4u64.to_le_bytes());
    cp[48..56].copy_from_slice(&4u64.to_le_bytes());
    repair_checkpoint_crc(&mut cp);
    assert!(matches!(rolling::read_ppu_suffix(&cp, &[], Limits::default()), Err(Error::Checkpoint(_))),
        "even an incomplete suffix cannot excuse prefix counters incompatible with original framing");
}

#[test]
fn prefix_frame_skip_permission_requires_actual_prefix_loss() {
    let f = fixture(); let (mut cp, tail) = checkpoint(&f, 6);
    let bits = section(&cp, 2) + 16 + 104;
    let flags = u32::from_le_bytes(cp[bits..bits + 4].try_into().unwrap());
    cp[bits..bits + 4].copy_from_slice(&(flags | 8).to_le_bytes());
    repair_checkpoint_crc(&mut cp);
    assert!(matches!(rolling::read_ppu_suffix(&cp, &tail, Limits::default()), Err(Error::Checkpoint(_))),
        "gap_since_frame cannot grant frame skips without any prefix GAP");
}

#[test]
fn malformed_record_counts_fail_before_collecting_later_chunks() {
    let original = fixture(); let (cp, _) = checkpoint(&original, 6);
    for (n, rule) in [(0u32, Rule::RecordsCountZero), (1u32, Rule::RecordsLength)] {
        let mut f = original.clone();
        let mut payload = Buf::default(); payload.u32(1).u32(n);
        f.chunks[6] = chunk(T_RECORDS, payload.0);
        // If scan buffers malformed RECORDS before validating their fixed row
        // count/length, this later unsupported chunk masks the local error.
        f.chunks[7] = chunk(40, vec![]);
        expect_rule(rolling::read_ppu_suffix(&cp, &f.bytes()[cut(&f, 6)..], Limits::default()), rule);
    }
}

#[test]
fn independent_resource_limits_refuse_without_relaxing_validation() {
    let f = fixture(); let (cp, tail) = checkpoint(&f, 6);
    let mut limits = Limits::default(); limits.checkpoint_bytes = cp.len() - 1;
    assert!(matches!(rolling::checkpoint_ppu(&f.bytes()[..cut(&f, 6)], limits), Err(Error::Limit(_))));
    assert!(matches!(rolling::read_ppu_suffix(&cp, &tail, limits), Err(Error::Limit(_))));
    limits = Limits::default(); limits.incarnations = 0;
    assert!(matches!(rolling::checkpoint_ppu(&f.bytes()[..cut(&f, 6)], limits), Err(Error::Limit(_))));
    limits = Limits::default(); limits.work_records = 2;
    assert!(matches!(rolling::checkpoint_ppu(&f.bytes()[..cut(&f, 6)], limits), Err(Error::Limit(_))));
    limits = Limits::default(); limits.stack_rows = 0;
    assert!(matches!(rolling::checkpoint_ppu(&f.bytes()[..cut(&f, 6)], limits), Err(Error::Limit(_))));
}
