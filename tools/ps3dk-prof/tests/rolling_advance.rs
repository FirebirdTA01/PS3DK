//! Canonical checkpoint transitions, independently framed original fixtures.
#[allow(dead_code)]
mod common;
use common::*;
use ps3dk_prof::rolling::{self, Error, Limits};
use ps3dk_prof::model::{CHUNK_WINDOW_BEGIN, CHUNK_WINDOW_END, FLAG_WINDOWS};
use ps3dk_prof::Rule;

use ps3dk_prof::rolling::advance_ppu;

fn offset(f: &File, chunks: usize) -> usize {
    16 + f.chunks[..chunks].iter().map(|c| c.payload.len() + 20).sum::<usize>()
}
fn direct(f: &File, chunks: usize) -> Vec<u8> {
    rolling::checkpoint_ppu(&f.bytes()[..offset(f, chunks)], Limits::default()).unwrap()
}
fn payload(f: &File, first: usize, end: usize) -> Vec<u8> {
    f.bytes()[offset(f, first)..offset(f, end)].to_vec()
}
fn window_fixture() -> File {
    let a = vec![frame(0, 1000, 0), begin(1, 0, 0, 1010, 1), bparent(2, 0, 0)];
    let b = vec![begin(3, 0, 1, 1020, 2), bparent(4, 1, 1), end(5, 0, 1, 1030, 2), end(6, 0, 0, 1040, 1)];
    let c = vec![gap(7, 8), frame(9, 1050, 3), regerr(10, 1)];
    let d = vec![begin(11, 0, 0, 1060, 3), bparent(12, 0, 0), end(13, 0, 0, 1070, 3)];
    let all = [a.clone(), b.clone(), c.clone(), d.clone()].concat();
    let mut l = lane(1, all); l.regerr = 1;
    let mut f = scenario(vec![l]).file(); let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    let mut begin = Buf::default();
    begin.u16(1).u16(0).u32(1).u32(0).u64(0).u8(1).u8(1).u16(0).u32(0).u32(0);
    let mut end = Buf::default();
    end.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(3); end.0.extend_from_slice(&[0; 12]);
    f.flags = FLAG_WINDOWS;
    f.chunks.push(chunk(CHUNK_WINDOW_BEGIN, begin.0));
    for records in [a, b, c, d] { f.chunks.push(chunk(T_RECORDS, records_payload(1, &records))); }
    f.chunks.extend([chunk(CHUNK_WINDOW_END, end.0), trailer]);
    f
}
fn token_fixture() -> File {
    let owner_a = vec![frame(0, 1000, 0)]; let owner_b = vec![frame(1, 1100, 1)];
    let future = vec![begin(0, 0, 0, 1010, 1), bparent_tok(1, 0, 0, 1, 1), end(2, 0, 0, 1020, 1)];
    let past = vec![begin(3, 0, 0, 1110, 2), bparent_tok(4, 0, 0, 1, 0), end(5, 0, 0, 1120, 2)];
    let mut f = scenario(vec![lane(1, [owner_a.clone(), owner_b.clone()].concat()),
        lane(2, [future.clone(), past.clone()].concat())]).file();
    let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    for (inc, records) in [(1, owner_a), (2, future), (1, owner_b), (2, past)] {
        f.chunks.push(chunk(T_RECORDS, records_payload(inc, &records)));
    }
    f.chunks.push(trailer); f
}
fn retire_fixture() -> File {
    let rows = vec![begin(0, 0, 0, 1000, 1), bparent(1, 0, 0), end(2, 0, 0, 1010, 1)];
    let mut old = lane(1, rows.clone()); old.state = 1;
    let mut f = scenario(vec![old.clone(), lane(2, rows.clone())]).file();
    let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(3);
    let mut one = thr(1); one.slot = 1;
    let mut two = thr(2); two.slot = 1;
    f.chunks.extend([chunk(T_THREADS, threads_payload(&[one])), chunk(T_RECORDS, records_payload(1, &rows)),
        chunk(T_RETIRED, retired_payload(&[old.fin()])), chunk(T_THREADS, threads_payload(&[two])),
        chunk(T_RECORDS, records_payload(2, &rows)), trailer]); f
}
fn assert_cuts(f: &File) {
    assert!(reader::read(&f.bytes()).unwrap().incomplete.is_empty(), "independent fixture must be valid");
    let last = f.chunks.len() - 1;
    for stride in [1, 2, 3, last] {
        let mut cp = direct(f, 0); let mut first = 0;
        while first < last {
            let end = (first + stride).min(last);
            cp = advance_ppu(&cp, &payload(f, first, end), Limits::default()).expect("canonical advancement");
            assert_eq!(cp, direct(f, end), "original checkpoint differs at chunks {first}..{end}");
            first = end;
        }
        let got = rolling::read_ppu_suffix(&cp, &payload(f, last, last + 1), Limits::default()).unwrap();
        assert!(got.incomplete.is_empty());
    }
}

#[test]
fn repeated_advancement_is_canonical_across_open_windows_stacks_and_real_gaps() {
    assert_cuts(&window_fixture());
}
#[test]
fn pending_closed_prefix_tokens_resolve_and_prefix_frame_runs_merge_exactly() {
    assert_cuts(&token_fixture());
}
#[test]
fn retirement_and_reused_slot_preserve_canonical_incarnation_state() {
    assert_cuts(&retire_fixture());
}
#[test]
fn empty_advancement_is_byte_identity_at_every_boundary() {
    let f = window_fixture();
    for n in 0..f.chunks.len() {
        let cp = direct(&f, n);
        assert_eq!(advance_ppu(&cp, &[], Limits::default()).unwrap(), cp);
    }
}
#[test]
fn frame_membership_is_merged_before_charging_unresolved_token_budget() {
    let f = token_fixture(); let cp = direct(&f, 7);
    let limits = Limits { frame_runs: 1, ..Limits::default() };
    // One prefix frame run 0..1 already proves this new token for frame 0.
    let next = advance_ppu(&cp, &payload(&f, 7, 8), limits).unwrap();
    assert_eq!(next, direct(&f, 8));
}
#[test]
fn work_budget_is_per_segment_but_cumulative_metadata_still_has_a_cap() {
    let f = window_fixture(); let cp = direct(&f, 6);
    let limits = Limits { work_records: 4, ..Limits::default() };
    let next = advance_ppu(&cp, &payload(&f, 6, 7), limits).unwrap();
    assert_eq!(next, direct(&f, 7)); // seven lifetime rows exceed this operation's cap
    let limits = Limits { work_records: 3, ..Limits::default() };
    assert!(matches!(advance_ppu(&cp, &payload(&f, 6, 7), limits), Err(Error::Limit(_))));
    let start = direct(&f, 4);
    // Prefix metadata uses 16 + 4-byte lengths + original framed chunks.
    let metadata = 16 + f.chunks[..4].iter().map(|c| c.payload.len() + 24).sum::<usize>();
    let limits = Limits { metadata_bytes: metadata, ..Limits::default() };
    assert!(matches!(advance_ppu(&start, &payload(&f, 4, 5), limits), Err(Error::Limit(_))));
}
#[test]
fn terminal_torn_and_malformed_segments_cannot_produce_next_checkpoints() {
    let f = window_fixture(); let cp = direct(&f, 6);
    let mut torn = payload(&f, 6, 7); torn.pop();
    assert!(matches!(advance_ppu(&cp, &torn, Limits::default()), Err(Error::Checkpoint(_))));
    let last = f.chunks.len() - 1;
    assert!(matches!(advance_ppu(&direct(&f, last), &payload(&f, last, last + 1), Limits::default()), Err(Error::Checkpoint(_))));
    let mut bad = f.clone(); bad.bad_crc = Some(6);
    assert!(matches!(advance_ppu(&cp, &payload(&bad, 6, 7), Limits::default()), Err(Error::Malformed(m)) if m.rule == Rule::ChunkCrc));
    let mut bad = f.clone(); bad.seq_override = Some((6, 0));
    assert!(matches!(advance_ppu(&cp, &payload(&bad, 6, 7), Limits::default()), Err(Error::Malformed(m)) if m.rule == Rule::ChunkSeqOrder));
    let mut bad = f; bad.chunks[6] = chunk(40, vec![]);
    assert!(matches!(advance_ppu(&cp, &payload(&bad, 6, 7), Limits::default()), Err(Error::Unsupported { chunk: Some(40), .. })));
}
