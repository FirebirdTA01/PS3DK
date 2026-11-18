//! Offline mixed-family joins through independently framed physical stores.
//! Original-source comparisons include causal support only, never tick lifting
//! of a compacted prefix. Checkpoints here are the already-tested Rust codec;
//! the independent C semantic producer is a separate artifact parity gate.
#[allow(dead_code)]
mod common;
mod spu_support;
use common::*;
use spu_support::*;
use std::collections::BTreeMap;
use ps3dk_prof::{Analysis, Outcome};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::ext::rolling::causal::{Bounds, CausalLimits, CounterHistory, PlacementBasis, CAUSAL_ONLY_NOTE};
use ps3dk_prof::rolling_combined::{self as combined, Limits, ReportOptions, RetainedCombined};
use ps3dk_prof::rolling_file::{self, CombinedError, CombinedFile};
use ps3dk_prof::{rolling_store, spu_ticks};
use ps3dk_prof::spu_ticks::{ObsKind, Options, SpuResult};

const PAGE: usize = 4096;
const PREFIX: usize = 3 * PAGE;
fn w32(b: &mut [u8], at: usize, n: u32) { b[at..at + 4].copy_from_slice(&n.to_le_bytes()); }
fn w64(b: &mut [u8], at: usize, n: u64) { b[at..at + 8].copy_from_slice(&n.to_le_bytes()); }
fn u32_at(b: &[u8], at: usize) -> u32 { u32::from_le_bytes(b[at..at + 4].try_into().unwrap()) }
fn checksum(b: &mut [u8], at: usize) { let c = crc32(&b[..at]); w32(b, at, c); }

#[derive(Clone, Copy)]
struct Layout { version: u32, retained: usize, checkpoint: usize, payload: usize, slot: usize, file: usize }
impl Layout {
    fn new(version: u32, retained: usize, checkpoint: usize, payload: usize) -> Self {
        let slot = (96 + checkpoint + payload + 64 + PAGE - 1) / PAGE * PAGE;
        Self { version, retained, checkpoint, payload, slot, file: PREFIX + (retained + 1) * slot }
    }
    fn ordinary(version: u32) -> Self { Self::new(version, 3, 32768, 8192) }
    fn limits(self) -> rolling_store::Limits {
        rolling_store::Limits { max_file_bytes: self.file as u64, max_slot_bytes: self.slot as u32 }
    }
}
#[derive(Clone)]
struct Part { checkpoint: Vec<u8>, payload: Vec<u8>, first: u64, next: u64 }
fn offset(f: &File, cut: usize) -> usize { 16 + f.chunks[..cut].iter().map(|c| c.payload.len() + 20).sum::<usize>() }
fn direct(f: &File, cut: usize) -> Vec<u8> {
    combined::checkpoint_combined(&f.bytes()[..offset(f, cut)], Limits::default()).expect("valid original prefix")
}
fn parts(f: &File, cuts: &[usize]) -> Vec<Part> {
    let bytes = f.bytes();
    cuts.windows(2).map(|p| Part { checkpoint: direct(f, p[0]),
        payload: bytes[offset(f, p[0])..offset(f, p[1])].to_vec(), first: p[0] as u64, next: p[1] as u64 }).collect()
}

// The wire builder shares no root/slot encoding implementation with the reader.
fn slot(bytes: &mut [u8], layout: Layout, id: usize, part: &Part) {
    assert!(part.checkpoint.len() <= layout.checkpoint && part.payload.len() <= layout.payload);
    let at = PREFIX + id % (layout.retained + 1) * layout.slot;
    let b = &mut bytes[at..at + layout.slot]; b.fill(0);
    b[..8].copy_from_slice(b"PFRSLOT1"); b[8..24].fill(0x42);
    w64(b, 24, id as u64); w64(b, 32, id as u64 + 2);
    w64(b, 40, part.first); w64(b, 48, part.next);
    w32(b, 56, part.checkpoint.len() as u32); w32(b, 60, part.payload.len() as u32);
    let cc = crc32(&part.checkpoint); let pc = crc32(&part.payload);
    w32(b, 64, cc); w32(b, 68, pc); checksum(b, 80);
    b[96..96 + part.checkpoint.len()].copy_from_slice(&part.checkpoint);
    let start = 96 + part.checkpoint.len(); b[start..start + part.payload.len()].copy_from_slice(&part.payload);
    let at = layout.slot - 64; let content_crc = crc32(&b[..at]);
    let f = &mut b[at..]; f[..8].copy_from_slice(b"PFRCMIT1");
    w64(f, 8, id as u64); w64(f, 16, id as u64 + 2); w64(f, 24, part.first); w64(f, 32, part.next);
    w32(f, 40, cc); w32(f, 44, pc); w32(f, 48, content_crc); checksum(f, 60);
}
fn root(bytes: &mut [u8], layout: Layout, all: &[Part], end: usize, generation: u64, state: u32, reason: u32) {
    let at = (1 + generation as usize % 2) * PAGE;
    let b = &mut bytes[at..at + PAGE]; b.fill(0);
    b[..8].copy_from_slice(if layout.version == 1 { b"PFRROOT1" } else { b"PFRROOT2" });
    let first = end.saturating_sub(layout.retained);
    let first_chunk = if first < end { all[first].first } else { 0 };
    let next_chunk = if end > 0 { all[end - 1].next } else { 0 };
    let prefix_bytes = all[..first].iter().map(|p| p.payload.len() as u64).sum::<u64>();
    let bound_bytes = all[..end].iter().map(|p| p.payload.len() as u64).sum::<u64>();
    w64(b, 8, generation); w32(b, 16, (end - first) as u32);
    w64(b, 24, first as u64); w64(b, 32, end as u64); w64(b, 40, first_chunk); w64(b, 48, next_chunk);
    w64(b, 56, first as u64); w64(b, 64, first_chunk); w64(b, 72, prefix_bytes); b[80..96].fill(0x42);
    let head = if layout.version == 1 { 96 } else {
        w32(b, 96, state); w32(b, 100, reason); w64(b, 104, next_chunk); w64(b, 112, bound_bytes); 128
    };
    for (n, id) in (first..end).enumerate() {
        let p = &all[id]; let at = head + n * 40;
        w32(b, at, (id % (layout.retained + 1)) as u32); w32(b, at + 4, p.checkpoint.len() as u32);
        w32(b, at + 8, p.payload.len() as u32); w64(b, at + 16, id as u64);
        w64(b, at + 24, p.first); w64(b, at + 32, p.next);
    }
    checksum(b, PAGE - 4);
}
fn container(all: &[Part], layout: Layout, state: u32, reason: u32) -> Vec<u8> {
    let mut b = vec![0; layout.file]; b[..8].copy_from_slice(b"PS3DKROL");
    w32(&mut b, 8, layout.version); w32(&mut b, 12, PAGE as u32);
    w64(&mut b, 16, layout.file as u64); w64(&mut b, 24, layout.file as u64);
    w32(&mut b, 32, layout.retained as u32); w32(&mut b, 36, layout.checkpoint as u32);
    w32(&mut b, 40, layout.payload as u32); w32(&mut b, 44, layout.slot as u32);
    b[48..64].fill(0x42); checksum(&mut b[..PAGE], PAGE - 4);
    for id in all.len().saturating_sub(layout.retained + 1)..all.len() { slot(&mut b, layout, id, &all[id]); }
    let n = all.len();
    if layout.version == 2 && state != 1 {
        root(&mut b, layout, all, n, n as u64 + 1, 1, 0);
        root(&mut b, layout, all, n, n as u64 + 2, state, reason);
    } else {
        if n > 0 { root(&mut b, layout, all, n - 1, n as u64, 1, 0); }
        root(&mut b, layout, all, n, n as u64 + 1, 1, 0);
    }
    rolling_store::read(&b, layout.limits()).expect("independent physical fixture must validate"); b
}

// Twelve segments; the two open zones cross the first retained boundary at
// segment 9. A new complete zone in each retained slot exposes accidental
// checkpoint re-import/report-baseline resets between slots.
fn fixture(early_name: &str) -> (File, Vec<usize>) {
    let mut groups = Vec::new();
    for g in 0..8u64 {
        groups.push(vec![begin(3 * g, 0, 0, 1000 + 100 * g, g + 1), bparent(3 * g + 1, 0, 0),
            end(3 * g + 2, 0, 0, 1010 + 100 * g, g + 1)]);
    }
    groups.push(vec![begin(24, 1, 0, 1800, 9), bparent(25, 0, 0)]);
    groups.push(vec![end(26, 1, 0, 1910, 9), begin(27, 2, 0, 1920, 10), bparent(28, 0, 0), end(29, 2, 0, 1940, 10)]);
    groups.push(vec![begin(30, 3, 0, 2000, 11), bparent(31, 0, 0), end(32, 3, 0, 2030, 11)]);
    groups.push(vec![begin(33, 4, 0, 2100, 12), bparent(34, 0, 0), end(35, 4, 0, 2140, 12)]);
    let mut scenario = scenario(vec![lane(1, groups.concat())]);
    scenario.descs = [early_name, "crossing", "retained-a", "retained-b", "retained-c"].iter()
        .enumerate().map(|(id, name)| desc(id as u32, name, "mixed-file.c")).collect();
    let mut f = scenario.file(); let trailer = f.chunks.pop().unwrap(); f.chunks.truncate(4);
    f.chunks.insert(2, chunk(T_PROFILE, Prof::default().payload())); f.flags = 7;
    let mut c = Cap::new(1, 1000);
    c.ext.iter_mut().find(|c| c.ty == T_LANES).unwrap().payload[16..20].copy_from_slice(&1u32.to_le_bytes());
    c.epoch(1, 0, 1001); c.bracket(1, 100000, 1100, 1110, Some(0));
    let mut cuts = vec![0];
    for (g, records) in groups.iter().enumerate() {
        c.ext.push(chunk(T_RECORDS, records_payload(1, records))); let raw = 100000 - g as u32 * 100;
        match g {
            8 => c.events(1, &[(raw - 10, D_WORK, BEGIN, 0)]),
            9 => c.events(1, &[(raw - 10, D_WORK, END, 0), (raw - 20, D_PING, BEGIN, 0), (raw - 30, D_PING, END, 0)]),
            _ => { let d = if g < 8 { D_WORK } else if g == 10 { D_PING } else { D_OTHER };
                c.events(1, &[(raw - 10, d, BEGIN, 0), (raw - 20, d, END, 0)]); },
        }
        let before = c.lane(1).next; let tb = 1200 + g as u64 * 200;
        c.bracket(1, raw - 40, tb, tb + 10, Some(before)); c.drained(1, tb + 50);
        if g < 11 { cuts.push(5 + c.ext.len()); }
    }
    c.finish(1); f.chunks.extend(c.ext); f.chunks.push(trailer); cuts.push(f.chunks.len()); (f, cuts)
}
fn full(f: &File) -> (Box<Analysis>, SpuResult) {
    let a = match load(&f.bytes()) { Outcome::Analysed(a) => a, Outcome::Malformed(m) => panic!("original fixture: {m}") };
    assert!(a.capture.incomplete.is_empty(), "ordinary full-source validation must precede join assertions");
    let causal = spu_ticks::analyze(&a, &Options { table: test_table(), ..Options::default() }).unwrap(); (a, causal)
}
fn read(b: &[u8], layout: Layout, limits: Limits) -> Result<CombinedFile, CombinedError> {
    let table: Vec<_> = test_table().into_iter().map(|e| e.entry).collect();
    rolling_file::read_combined(b, layout.limits(), limits,
        ReportOptions { spu_profile_table: &table, causal_limits: CausalLimits::default() })
}
fn standalone(cp: &[u8], payload: &[u8], limits: Limits) -> Result<RetainedCombined, combined::Error> {
    let table: Vec<_> = test_table().into_iter().map(|e| e.entry).collect();
    combined::read_combined_suffix(cp, payload, limits,
        ReportOptions { spu_profile_table: &table, causal_limits: CausalLimits::default() })
}
fn assert_limit(result: Result<CombinedFile, CombinedError>, expected: &str) {
    match result { Err(CombinedError::Limit(got)) | Err(CombinedError::Semantic(combined::Error::Limit(got))) => assert_eq!(got, expected),
        Err(e) => panic!("expected limit {expected}, got {e:?}"), Ok(_) => panic!("expected limit {expected}, accepted") }
}
fn assert_report(got: &RetainedCombined, f: &File, cut: usize, full: &Analysis, causal: &SpuResult) {
    assert_eq!(got.original_header.as_slice(), &f.bytes()[..16]); assert_eq!(got.original_profile, full.capture.profile);
    assert!(got.terminal && !got.torn_tail && got.ppu.incomplete.is_empty());
    assert_eq!((got.first_chunk, got.next_chunk, got.prefix_bytes), (cut as u64, f.chunks.len() as u64, (offset(f, cut) - 16) as u64));
    let p = f.chunks[..cut].iter().filter(|c| c.ty == T_RECORDS).map(|c| u64::from(u32_at(&c.payload, 4))).sum::<u64>();
    let s = f.chunks[..cut].iter().filter(|c| c.ty == T_SPU).map(|c| u64::from(u32_at(&c.payload, 16))).sum::<u64>();
    assert_eq!((got.ppu.lanes[&1].prefix_written, got.ppu.lanes[&1].retained_written), (p, 36 - p));
    let lane = &got.spu.retained.sources[&SourceKey::Lane(1)];
    assert_eq!((lane.prefix_events, lane.retained_events), (s, 24 - s));
    assert_eq!(lane.events.iter().map(|e| e.seq).collect::<Vec<_>>(), (s..24).collect::<Vec<_>>());
    assert_eq!(got.spu.causal.basis, PlacementBasis::CausalSupportOnly);
    assert_eq!(got.spu.causal.counter_history, CounterHistory::NotEvaluated); assert_eq!(got.spu.causal.note, CAUSAL_ONLY_NOTE);
    let expected: BTreeMap<_, _> = causal.lanes[0].obs.iter().filter_map(|o| match o.kind {
        ObsKind::Event { seq, .. } => Some((seq, Bounds { lower: o.l, upper_drain: o.u1, upper_bracket: o.u2, upper: o.u })), _ => None }).collect();
    let endpoints = &got.spu.causal.lanes[&1].endpoints;
    assert_eq!(endpoints.iter().map(|e| e.seq).collect::<Vec<_>>(), (s..24).collect::<Vec<_>>());
    for e in endpoints { assert_eq!(e.bounds, expected[&e.seq], "original cut {cut}, SPU seq {}", e.seq); }
    for (id, name) in [(0, "evicted"), (1, "crossing"), (2, "retained-a"), (3, "retained-b"), (4, "retained-c")] {
        let spans: Vec<_> = full.capture.spans.iter().filter(|s| s.desc == id && s.end_seq.is_some_and(|end| end >= p)).collect();
        let complete = spans.iter().filter(|s| s.begin_seq >= p).count() as u64;
        let left = spans.iter().filter(|s| s.begin_seq < p).count() as u64;
        let zone = got.ppu.zones.iter().find(|z| z.name == name);
        assert_eq!(zone.map_or((0, 0), |z| (z.completed, z.left_censored)), (complete, left), "{name} cut {cut}");
    }
}

#[test]
fn all_retained_slots_keep_counts_crossing_spans_and_original_causal_bounds() {
    let (f, cuts) = fixture("evicted"); let (a, causal) = full(&f); let p = parts(&f, &cuts);
    for version in [1, 2] {
        let layout = Layout::ordinary(version); let b = container(&p, layout, 1, 0);
        let got = read(&b, layout, Limits::default()).expect("combined physical join unavailable");
        assert_eq!((got.store_view.first_segment, got.store_view.next_segment, got.store_view.evicted_segments), (9, 12, 9));
        assert_eq!(got.store_view.state, if version == 1 { 0 } else { 1 });
        assert_report(&got.retained, &f, cuts[9], &a, &causal);
        for (name, ticks) in [("retained-a", 20), ("retained-b", 30), ("retained-c", 40)] {
            assert_eq!(got.retained.ppu.zones.iter().find(|z| z.name == name).unwrap().total_ticks, Some(ticks));
        }
        let source = &got.retained.spu.retained.sources[&SourceKey::Lane(1)];
        assert_eq!(source.left_censored.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(16, 17)]);
        assert_eq!(source.pairs.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(18, 19), (20, 21), (22, 23)]);
    }
}

#[test]
fn every_original_boundary_can_start_a_physically_retained_window() {
    let (f, _) = fixture("evicted"); let (a, causal) = full(&f);
    let layout = Layout::new(2, 2, 32768, 8192);
    for cut in 0..f.chunks.len() {
        let middle = cut + (f.chunks.len() - 1 - cut) / 2;
        let p = parts(&f, &[0, cut, middle, f.chunks.len()]); let b = container(&p, layout, 1, 0);
        let got = read(&b, layout, Limits::default()).expect("every-cut physical import");
        assert_report(&got.retained, &f, cut, &a, &causal);
    }
}

#[test]
fn repaired_crc_intermediate_forgery_rejects_without_semantic_root_fallback() {
    let (f, cuts) = fixture("evicted"); full(&f); let mut p = parts(&f, &cuts);
    let mut altered = f.clone();
    let index = altered.chunks.iter().enumerate().filter(|(_, c)| c.ty == T_RECORDS).nth(10).unwrap().0;
    // Change one otherwise-valid elapsed duration already retained in segment10.
    // Serialized cursor state binds last_tb even though that closed span is not
    // replayed as a prefix event. All physical and checkpoint CRCs are rebuilt.
    let records = vec![begin(30, 3, 0, 2000, 11), bparent(31, 0, 0), end(32, 3, 0, 2031, 11)];
    altered.chunks[index] = chunk(T_RECORDS, records_payload(1, &records)); full(&altered);
    p[11].checkpoint = direct(&altered, cuts[11]);
    standalone(&p[11].checkpoint, &p[11].payload, Limits::default()).expect("forgery is individually valid");
    assert_ne!(combined::advance_combined(&p[10].checkpoint, &p[10].payload, Limits::default()).unwrap(), p[11].checkpoint);
    let layout = Layout::ordinary(2); let b = container(&p, layout, 1, 0);
    assert_eq!(rolling_store::read(&b, layout.limits()).unwrap().view.next_segment, 12);
    match read(&b, layout, Limits::default()) { Err(CombinedError::Transition { segment: 11 }) => {},
        Err(e) => panic!("expected segment11 transition, got {e:?}"), Ok(_) => panic!("forged transition accepted") }
}

#[test]
fn first_original_chunk_frontier_must_match_selected_physical_root() {
    let (f, cuts) = fixture("evicted"); full(&f); let mut p = parts(&f, &cuts);
    p[9].checkpoint = direct(&f, cuts[9] + 1);
    let layout = Layout::ordinary(2); let b = container(&p, layout, 1, 0);
    match read(&b, layout, Limits::default()) { Err(CombinedError::Boundary { .. }) => {},
        Err(e) => panic!("expected first frontier boundary, got {e:?}"), Ok(_) => panic!("wrong first frontier accepted") }
}

#[test]
fn first_prefix_byte_frontier_binds_evicted_physical_payload_bytes() {
    let (f, cuts) = fixture("evicted"); full(&f); let mut p = parts(&f, &cuts);
    let (longer, other_cuts) = fixture("a longer evicted descriptor"); full(&longer); assert_eq!(cuts, other_cuts);
    for id in 9..12 { p[id].checkpoint = direct(&longer, cuts[id]); }
    assert_ne!(offset(&f, cuts[9]), offset(&longer, cuts[9]));
    standalone(&p[9].checkpoint, &p[9..].iter().flat_map(|p| p.payload.iter().copied()).collect::<Vec<_>>(), Limits::default()).unwrap();
    let layout = Layout::ordinary(2); let b = container(&p, layout, 1, 0);
    match read(&b, layout, Limits::default()) { Err(CombinedError::Boundary { .. }) => {},
        Err(e) => panic!("expected prefix-byte boundary, got {e:?}"), Ok(_) => panic!("wrong prefix bytes accepted") }
}

#[test]
fn empty_and_metadata_only_segments_preserve_canonical_state_and_retained_baseline() {
    let (f, cuts) = fixture("evicted"); let (a, causal) = full(&f); let mut p = parts(&f, &cuts);
    let moved = p[10].payload.clone(); p[9].payload.extend(moved); p[9].next = p[10].next;
    p[10] = Part { checkpoint: direct(&f, cuts[11]), payload: vec![], first: cuts[11] as u64, next: cuts[11] as u64 };
    let layout = Layout::ordinary(2); let b = container(&p, layout, 1, 0);
    assert_report(&read(&b, layout, Limits::default()).unwrap().retained, &f, cuts[9], &a, &causal);
    let middle = cuts[10] - 1; assert_eq!(f.chunks[middle].ty, T_ORDER);
    let p = parts(&f, &[0, cuts[9], middle, middle + 1, f.chunks.len()]);
    let layout = Layout::new(2, 3, 32768, 8192); let b = container(&p, layout, 1, 0);
    assert_report(&read(&b, layout, Limits::default()).unwrap().retained, &f, cuts[9], &a, &causal);
}

#[test]
fn trailer_before_an_empty_later_segment_is_not_a_valid_transition() {
    let (f, cuts) = fixture("evicted"); full(&f); let mut p = parts(&f, &cuts);
    let tail: Vec<_> = p[10..].iter().flat_map(|p| p.payload.iter().copied()).collect();
    p[9].payload.extend(tail); p[9].next = f.chunks.len() as u64;
    for part in &mut p[10..] { part.payload.clear(); part.first = f.chunks.len() as u64; part.next = part.first; }
    let layout = Layout::ordinary(2); let b = container(&p, layout, 2, 0);
    match read(&b, layout, Limits::default()) { Err(CombinedError::Semantic(combined::Error::Checkpoint(_))) => {},
        Err(e) => panic!("expected terminal transition refusal, got {e:?}"), Ok(_) => panic!("post-TRAILER segment accepted") }
}

#[test]
fn sealed_and_stopped_roots_do_not_manufacture_original_final_trailer() {
    let (f, cuts) = fixture("evicted"); full(&f); let mut p = parts(&f, &cuts);
    let without_trailer = p[11].payload.len() - (20 + f.chunks.last().unwrap().payload.len());
    p[11].payload.truncate(without_trailer); p[11].next -= 1;
    let layout = Layout::ordinary(2);
    for (state, reason) in [(2, 0), (3, 1), (3, 2), (3, 3), (3, 4), (3, 5)] {
        let b = container(&p, layout, state, reason); let got = read(&b, layout, Limits::default()).unwrap();
        assert_eq!((got.store_view.state, got.store_view.reason), (state, reason));
        assert!(!got.retained.terminal && !got.retained.torn_tail && !got.retained.ppu.incomplete.is_empty());
        assert_eq!(got.retained.ppu.lanes[&1].retained_written, 10);
    }
}

#[test]
fn sealed_physical_root_cannot_override_contradictory_original_family_finals() {
    let (mut f, cuts) = fixture("evicted"); full(&f);
    let final_index = f.chunks.iter().position(|c| c.ty == T_FINAL).unwrap();
    w64(&mut f.chunks[final_index].payload, 20, 25); // actual SPU event total is24
    let p = parts(&f, &cuts); let layout = Layout::ordinary(2); let b = container(&p, layout, 2, 0);
    match read(&b, layout, Limits::default()) {
        Err(CombinedError::Semantic(combined::Error::Spu(ps3dk_prof::ext::rolling::Error::Malformed(_)))) |
        Err(CombinedError::Semantic(combined::Error::Malformed(_))) => {},
        Err(e) => panic!("expected malformed original final, got {e:?}"), Ok(_) => panic!("bad source final accepted") }
}

#[test]
fn torn_newest_slot_uses_only_the_physically_selected_older_chain() {
    let (f, cuts) = fixture("evicted"); full(&f); let p = parts(&f, &cuts); let layout = Layout::ordinary(2);
    let mut b = container(&p, layout, 1, 0); b[PREFIX + (11 % 4) * layout.slot + 72] ^= 1;
    let view = rolling_store::read(&b, layout.limits()).unwrap().view;
    assert!(view.recovered); assert_eq!((view.first_segment, view.next_segment), (8, 11));
    let got = read(&b, layout, Limits::default()).unwrap(); assert!(got.store_view.recovered);
    assert!(!got.retained.terminal && !got.retained.ppu.incomplete.is_empty());
    assert_eq!((got.retained.first_chunk, got.retained.next_chunk), (cuts[8] as u64, cuts[11] as u64));
    assert_eq!(got.retained.ppu.lanes[&1].retained_written, 9);
    assert_eq!(got.retained.spu.retained.sources[&SourceKey::Lane(1)].retained_events, 6);
}

#[test]
fn empty_root_has_no_combined_semantic_checkpoint_even_when_sealed() {
    let layout = Layout::ordinary(2);
    for (state, reason) in [(1, 0), (2, 0), (3, 1)] {
        let b = container(&[], layout, state, reason);
        match read(&b, layout, Limits::default()) { Err(CombinedError::EmptyStore) => {},
            Err(e) => panic!("expected EmptyStore, got {e:?}"), Ok(_) => panic!("empty semantic report manufactured") }
    }
}

#[test]
fn event_work_is_cumulative_across_all_retained_slots() {
    let (f, cuts) = fixture("evicted"); full(&f); let p = parts(&f, &cuts); let layout = Layout::ordinary(2);
    for part in &p[9..] { standalone(&part.checkpoint, &part.payload, Limits { work_events: 7, ..Limits::default() }).unwrap(); }
    let b = container(&p, layout, 1, 0);
    assert_limit(read(&b, layout, Limits { work_events: 7, ..Limits::default() }), "event work");
}

#[test]
fn input_bytes_are_admitted_for_the_entire_borrowed_payload_chain() {
    let (f, cuts) = fixture("evicted"); full(&f); let p = parts(&f, &cuts); let layout = Layout::ordinary(2);
    let input_bytes = p[9..].iter().map(|p| p.payload.len()).max().unwrap();
    for part in &p[9..] { standalone(&part.checkpoint, &part.payload, Limits { input_bytes, ..Limits::default() }).unwrap(); }
    let b = container(&p, layout, 1, 0);
    assert_limit(read(&b, layout, Limits { input_bytes, ..Limits::default() }), "input bytes");
}

#[test]
fn terminal_slot_cannot_reset_cumulative_metadata_admission() {
    let (f, cuts) = fixture("evicted"); full(&f); let p = parts(&f, &cuts); let layout = Layout::ordinary(2);
    let charge = |from: usize, to: usize| f.chunks[from..to].iter().filter(|c| !matches!(c.ty, T_RECORDS | T_SPU))
        .map(|c| 24 + c.payload.len()).sum::<usize>();
    let prefix = 16 + charge(0, cuts[9]);
    let metadata_bytes = prefix + (9..12).map(|i| charge(cuts[i], cuts[i + 1])).max().unwrap();
    assert!(metadata_bytes < 16 + charge(0, f.chunks.len()));
    // First selected slot fits; further actual retained support must accumulate.
    standalone(&p[9].checkpoint, &p[9].payload, Limits { metadata_bytes, ..Limits::default() }).unwrap();
    let b = container(&p, layout, 1, 0);
    assert_limit(read(&b, layout, Limits { metadata_bytes, ..Limits::default() }), "metadata bytes");
}

#[test]
fn canonical_boundary_proof_reservations_share_one_operation_budget() {
    let (f, _) = fixture("evicted"); full(&f); let checkpoint = direct(&f, 0);
    let p = vec![Part { checkpoint: checkpoint.clone(), payload: vec![], first: 0, next: 0 }; 12];
    let defaults = Limits::default(); let limits = Limits { position_total_work: defaults.position_work * 2, ..defaults };
    assert_eq!(combined::advance_combined(&checkpoint, &[], limits).unwrap(), checkpoint);
    let layout = Layout::ordinary(2); let b = container(&p, layout, 1, 0);
    assert_limit(read(&b, layout, limits), "position work reservation");
}

#[test]
fn checkpoint_admission_applies_before_importing_selected_slot() {
    let (f, cuts) = fixture("evicted"); full(&f); let p = parts(&f, &cuts); let layout = Layout::ordinary(2);
    let b = container(&p, layout, 1, 0);
    assert_limit(read(&b, layout, Limits { checkpoint_bytes: 0, ..Limits::default() }), "checkpoint bytes");
}

#[test]
fn fresh_frame_membership_work_cannot_hide_across_slot_boundaries() {
    let mut frames = vec![frame(0, 1000, 0)];
    for n in 1..4096u64 { frames.extend([gap(2 * n - 1, 2 * n - 1), frame(2 * n, 1000 + n, 2 * n)]); }
    let mut spans = Vec::new();
    for n in 0..4096u64 { spans.extend([begin(3 * n, 0, 0, 10000 + 2 * n, n + 1),
        bparent_tok(3 * n + 1, 0, 0, 1, 1), end(3 * n + 2, 0, 0, 10001 + 2 * n, n + 1)]); }
    let mut f = scenario(vec![lane(1, frames), lane(2, spans)]).file(); f.flags = 1;
    let a = match load(&f.bytes()) { Outcome::Analysed(a) => a, Outcome::Malformed(m) => panic!("original resource fixture: {m}") };
    assert!(a.capture.incomplete.is_empty()); assert_eq!(a.capture.lanes[&1].frames.len(), 4096);
    assert_eq!(a.capture.spans.len(), 4096);
    let mut cuts = vec![0]; cuts.extend([4; 9]); cuts.extend([5, 6, 7]);
    assert_eq!(f.chunks.len(), 7); let bytes = f.bytes();
    // Construct individually valid boundary claims with sufficient admission;
    // only the physical join below receives the deliberately smaller budget.
    let p: Vec<_> = cuts.windows(2).map(|pair| Part {
        checkpoint: combined::checkpoint_combined(&bytes[..offset(&f, pair[0])],
            Limits { work_items: 64_000_000, ..Limits::default() }).unwrap(),
        payload: bytes[offset(&f, pair[0])..offset(&f, pair[1])].to_vec(),
        first: pair[0] as u64, next: pair[1] as u64,
    }).collect();
    let layout = Layout::new(2, 3, 262144, 524288); let b = container(&p, layout, 1, 0);
    assert_limit(read(&b, layout, Limits { work_items: 8_000_000, ..Limits::default() }), "structural work");
}
