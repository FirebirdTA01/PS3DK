//! Combined offline restart controls using independently framed mixed sources.
//! Full-source causal comparisons never tick-lift a compacted retained source.
#[allow(dead_code)]
mod common;
mod spu_support;
use common::*;
use spu_support::*;
use std::collections::BTreeMap;
use std::ops::Range;
use ps3dk_prof::{Analysis, Outcome, Rule};
use ps3dk_prof::ext::SourceKey;
use ps3dk_prof::ext::rolling::causal::{Bounds, CausalLimits, CounterHistory, PlacementBasis, CAUSAL_ONLY_NOTE};
use ps3dk_prof::model::{CHUNK_WINDOW_BEGIN, CHUNK_WINDOW_END, FLAG_WINDOWS};
use ps3dk_prof::profile::TableEntry;
use ps3dk_prof::rolling_combined::{self as combined, Error, Limits, ReportOptions, RetainedCombined};
use ps3dk_prof::spu_ticks::{self, ObsKind, Options, SpuResult};

fn window_begin() -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u32(0).u64(0).u8(1).u8(1).u16(0).u32(0).u32(0);
    chunk(CHUNK_WINDOW_BEGIN, b.0)
}
fn window_end(last: u64) -> Chunk {
    let mut b = Buf::default();
    b.u16(1).u16(0).u32(1).u8(1).u8(0).u16(0).u64(last);
    b.0.extend_from_slice(&[0; 12]); chunk(CHUNK_WINDOW_END, b.0)
}
fn owner(c: &mut Cap, incarnation: u32) {
    let lanes = c.ext.iter_mut().find(|c| c.ty == T_LANES).unwrap();
    lanes.payload[16..20].copy_from_slice(&incarnation.to_le_bytes());
}
fn assemble(mut ppu: File, c: Cap) -> File {
    let trailer = ppu.chunks.pop().unwrap();
    ppu.chunks.truncate(4);
    ppu.chunks.insert(2, chunk(T_PROFILE, Prof::default().payload()));
    ppu.flags = c.flags;
    ppu.chunks.extend(c.ext); ppu.chunks.push(trailer); ppu
}
fn fixture() -> File {
    let pre = vec![frame(0, 1000, 0), begin(1, 0, 0, 1010, 1), bparent(2, 0, 0)];
    let tail = vec![begin(3, 1, 1, 1020, 2), bparent(4, 1, 1), end(5, 1, 1, 1040, 2),
        end(6, 0, 0, 1050, 1), frame(7, 1100, 1), begin(8, 2, 0, 1110, 3), bparent(9, 0, 0),
        end(10, 2, 0, 1140, 3), frame(11, 1200, 2)];
    let mut f = scenario(vec![lane(1, [pre.clone(), tail.clone()].concat()), lane(2, vec![])]).file();
    // Owner 2 genuinely appears later, inside a support-free SPU event block.
    f.chunks[3] = chunk(T_THREADS, threads_payload(&[thr(1)]));
    let mut c = Cap::new(1, 1000); owner(&mut c, 1); c.flags |= FLAG_WINDOWS;
    c.epoch(1, 0, 1001); c.bracket(1, 10000, 1100, 1110, Some(0));
    c.ext.extend([window_begin(), chunk(T_RECORDS, records_payload(1, &pre))]);
    c.events(1, &[(9990, D_WORK, BEGIN, 0)]);
    c.ext.push(chunk(T_THREADS, threads_payload(&[thr(2)])));
    c.events(1, &[(9980, D_PING, BEGIN, 1), (9970, D_PING, END, 1), (9960, D_WORK, END, 0)]);
    c.ext.push(chunk(T_RECORDS, records_payload(1, &tail)));
    c.bracket(1, 9950, 1200, 1210, Some(4)); c.drained(1, 1250);
    c.events(1, &[(9940, D_OTHER, BEGIN, 0), (9930, D_OTHER, END, 0)]);
    c.bracket(1, 9920, 1300, 1310, Some(6)); c.drained(1, 1350); c.finish(1);
    c.ext.push(window_end(2)); assemble(f, c)
}
fn offset(f: &File, count: usize) -> usize {
    16 + f.chunks[..count].iter().map(|c| c.payload.len() + 20).sum::<usize>()
}
fn direct(f: &File, count: usize) -> Vec<u8> {
    combined::checkpoint_combined(&f.bytes()[..offset(f, count)], Limits::default()).expect("combined checkpoint")
}
fn table() -> Vec<TableEntry> { test_table().into_iter().map(|e| e.entry).collect() }
fn read(cp: &[u8], suffix: &[u8], limits: Limits) -> Result<RetainedCombined, Error> {
    combined::read_combined_suffix(cp, suffix, limits,
        ReportOptions { spu_profile_table: &table(), causal_limits: CausalLimits::default() })
}
fn full_source(f: &File) -> (Box<Analysis>, SpuResult) {
    let a = match load(&f.bytes()) {
        Outcome::Analysed(a) => a, Outcome::Malformed(m) => panic!("invalid original mixed fixture: {m}"),
    };
    assert!(a.capture.incomplete.is_empty(), "full original source must be structurally complete");
    let result = spu_ticks::analyze(&a, &Options { table: test_table(), ..Options::default() }).unwrap();
    (a, result)
}
fn nth(f: &File, ty: u32, n: usize) -> usize {
    f.chunks.iter().enumerate().filter(|(_, c)| c.ty == ty).nth(n).unwrap().0
}
fn u32_at(b: &[u8], n: usize) -> u32 { u32::from_le_bytes(b[n..n + 4].try_into().unwrap()) }
fn u64_at(b: &[u8], n: usize) -> u64 { u64::from_le_bytes(b[n..n + 8].try_into().unwrap()) }
fn put64(b: &mut [u8], n: usize, v: u64) { b[n..n + 8].copy_from_slice(&v.to_le_bytes()); }
fn repair(cp: &mut [u8]) {
    let end = cp.len() - 4; let crc = crc32(&cp[..end]); cp[end..].copy_from_slice(&crc.to_le_bytes());
}
fn repair_raw(cp: &mut [u8], raw: Range<usize>) {
    let crc = crc32(&cp[raw.start..raw.end - 4]); cp[raw.end - 4..raw.end].copy_from_slice(&crc.to_le_bytes());
    repair(cp);
}
fn sections(cp: &[u8]) -> Vec<(u32, Range<usize>)> {
    assert_eq!(&cp[..8], b"PFRMIX01"); assert_eq!(cp[8..12], [1, 0, 80, 0]);
    assert_eq!(u32_at(cp, 12), 4); assert_eq!(u32_at(cp, 56), 3);
    assert_eq!(&cp[60..64], &[0; 4]); assert_eq!(&cp[64..72], &[1, 0, 1, 0, 1, 0, 1, 0]);
    assert_eq!(&cp[72..80], &[0; 8]);
    assert_eq!(u64_at(cp, 48) as usize, cp.len() - 84);
    assert_eq!(crc32(&cp[..cp.len() - 4]), u32_at(cp, cp.len() - 4));
    let mut at = 80; let mut out = Vec::new();
    for kind in 1..=4u16 {
        assert_eq!(u16::from_le_bytes(cp[at..at + 2].try_into().unwrap()), kind);
        assert_eq!(&cp[at + 2..at + 4], &[1, 0]); assert_eq!(u32_at(cp, at + 12), 0);
        let count = u32_at(cp, at + 4); let end = at + 16 + u32_at(cp, at + 8) as usize;
        out.push((count, at + 16..end)); at = end;
    }
    assert_eq!(at, cp.len() - 4); out
}
fn metadata(cp: &[u8]) -> Vec<Range<usize>> {
    let (count, body) = sections(cp)[0].clone(); let mut at = body.start; let mut out = Vec::new();
    for _ in 0..count { let n = u32_at(cp, at) as usize; out.push(at + 4..at + 4 + n); at += 4 + n; }
    assert_eq!(at, body.end); out
}
fn meta(cp: &[u8], ty: u32, n: usize) -> Range<usize> {
    metadata(cp).into_iter().filter(|r| u32_at(cp, r.start) == ty).nth(n).unwrap()
}
fn blocks(cp: &[u8]) -> Vec<Range<usize>> {
    let (count, body) = sections(cp)[3].clone(); let mut at = body.start; let mut out = Vec::new();
    for _ in 0..count {
        let len = u32_at(cp, at + 4) as usize;
        if u32_at(cp, at) == 2 { out.push(at..at + len); }
        at += len;
    }
    assert_eq!(at, body.end); out
}
fn reject<T>(result: Result<T, Error>) {
    match result {
        Err(Error::MissingImplementation) => panic!("combined rejection not implemented"),
        Err(_) => {}, Ok(_) => panic!("malformed combined claim was accepted"),
    }
}
fn malformed<T>(result: Result<T, Error>, rule: Rule) {
    match result {
        Err(Error::Malformed(m)) | Err(Error::Spu(ps3dk_prof::ext::rolling::Error::Malformed(m))) => assert_eq!(m.rule, rule),
        Err(e) => panic!("expected {rule:?}, got {e:?}"), Ok(_) => panic!("expected {rule:?}, accepted"),
    }
}
fn limit<T>(result: Result<T, Error>, name: &str) {
    match result { Err(Error::Limit(got)) => assert_eq!(got, name),
        Err(e) => panic!("expected limit {name}, got {e:?}"), Ok(_) => panic!("expected limit {name}, accepted") }
}

#[test]
fn every_mixed_cut_preserves_retained_counts_spans_and_full_source_causal_bounds() {
    let f = fixture(); let (full, causal) = full_source(&f); let bytes = f.bytes();
    let expected: BTreeMap<u64, Bounds> = causal.lanes[0].obs.iter().filter_map(|o| match o.kind {
        ObsKind::Event { seq, .. } => Some((seq, Bounds { lower: o.l, upper_drain: o.u1, upper_bracket: o.u2, upper: o.u })),
        _ => None,
    }).collect();
    for cut in 0..f.chunks.len() {
        let cp = direct(&f, cut); let got = read(&cp, &bytes[offset(&f, cut)..], Limits::default()).unwrap();
        assert_eq!(got.original_header.as_slice(), &bytes[..16]);
        assert_eq!(got.original_profile, full.capture.profile);
        assert!(got.terminal && !got.torn_tail && got.ppu.incomplete.is_empty());
        assert_eq!((got.first_chunk, got.next_chunk, got.prefix_bytes),
            (cut as u64, f.chunks.len() as u64, (offset(&f, cut) - 16) as u64));
        let p: u64 = f.chunks[..cut].iter().filter(|c| c.ty == T_RECORDS).map(|c| u64::from(u32_at(&c.payload, 4))).sum();
        let s: u64 = f.chunks[..cut].iter().filter(|c| c.ty == T_SPU).map(|c| u64::from(u32_at(&c.payload, 16))).sum();
        assert_eq!((got.ppu.lanes[&1].prefix_written, got.ppu.lanes[&1].retained_written), (p, 12 - p));
        let source = &got.spu.retained.sources[&SourceKey::Lane(1)];
        assert_eq!((source.prefix_events, source.retained_events), (s, 6 - s));
        assert_eq!(source.events.iter().map(|e| e.seq).collect::<Vec<_>>(), (s..6).collect::<Vec<_>>());
        assert!(got.spu.causal.refusal.is_none());
        assert_eq!(got.spu.causal.basis, PlacementBasis::CausalSupportOnly);
        assert_eq!(got.spu.causal.counter_history, CounterHistory::NotEvaluated);
        assert_eq!(got.spu.causal.note, CAUSAL_ONLY_NOTE);
        let endpoints = &got.spu.causal.lanes[&1].endpoints;
        assert_eq!(endpoints.iter().map(|e| e.seq).collect::<Vec<_>>(), (s..6).collect::<Vec<_>>());
        for endpoint in endpoints { assert_eq!(endpoint.bounds, expected[&endpoint.seq], "cut {cut}"); }
        for (desc, name) in [(0, "zoneA"), (1, "zoneB"), (2, "zoneC")] {
            let spans: Vec<_> = full.capture.spans.iter().filter(|x| x.desc == desc && x.end_seq.is_some_and(|e| e >= p)).collect();
            let complete = spans.iter().filter(|x| x.begin_seq >= p).count() as u64;
            let left = spans.iter().filter(|x| x.begin_seq < p).count() as u64;
            let zone = got.ppu.zones.iter().find(|z| z.name == name);
            assert_eq!(zone.map_or((0, 0), |z| (z.completed, z.left_censored)), (complete, left), "{name} cut {cut}");
        }
    }
}

#[test]
fn crossing_boundaries_keep_open_window_and_censor_both_family_spans() {
    let f = fixture(); full_source(&f); let cut = nth(&f, T_SPU, 0) + 1;
    let got = read(&direct(&f, cut), &f.bytes()[offset(&f, cut)..], Limits::default()).unwrap();
    let outer = got.ppu.zones.iter().find(|z| z.name == "zoneA").unwrap();
    assert_eq!((outer.completed, outer.left_censored, outer.total_ticks), (0, 1, None));
    assert!(got.ppu.windows[0].left_clipped);
    let spu = &got.spu.retained.sources[&SourceKey::Lane(1)];
    assert_eq!(spu.left_censored.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(0, 3)]);
    assert_eq!(spu.pairs.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(1, 2), (4, 5)]);
    assert!(got.spu.causal.lanes[&1].endpoints.iter().all(|e| e.seq != 0));
}

#[test]
fn combined_wire_is_exact_original_header_shared_metadata_and_per_incarnation_chunk_counts() {
    let f = fixture(); full_source(&f); let cut = f.chunks.len() - 1; let cp = direct(&f, cut);
    let sec = sections(&cp); assert_eq!(&cp[16..32], &f.bytes()[..16]);
    assert_eq!((u64_at(&cp, 32), u64_at(&cp, 40)), (cut as u64, (offset(&f, cut) - 16) as u64));
    let raws = metadata(&cp); let expected: Vec<_> = f.chunks[..cut].iter().enumerate()
        .filter(|(_, c)| !matches!(c.ty, T_RECORDS | T_SPU)).map(|(i, _)| i).collect();
    assert_eq!(raws.len(), expected.len());
    let bytes = f.bytes();
    for (raw, i) in raws.iter().zip(expected) { assert_eq!(&cp[raw.clone()], &bytes[offset(&f, i)..offset(&f, i + 1)]); }
    let mut at = sec[1].1.start; let mut p = 0; let mut r = 0;
    for _ in 0..sec[1].0 {
        let len = u32_at(&cp, at) as usize; let inc = u32_at(&cp, at + 4);
        assert_eq!(len, 128 + 80 * u32_at(&cp, at + 108) as usize + 16 * u32_at(&cp, at + 112) as usize);
        let chunks = u64_at(&cp, at + 120);
        assert_eq!(chunks, if inc == 1 { 2 } else { 0 }); p += chunks;
        r += u64_at(&cp, at + 8) + u64_at(&cp, at + 24); at += len;
    }
    assert_eq!(at, sec[1].1.end);
    let mut s = 0; let mut e = 0;
    for block in blocks(&cp) {
        let count = u32_at(&cp, block.start + 24); let mut run = block.start + 32;
        for _ in 0..count {
            e += u64_at(&cp, run + 16); s += u64_at(&cp, run + 24);
            run += 64 + 24 * u32_at(&cp, run + 36) as usize;
        }
        assert_eq!(run, block.end);
    }
    let l: u64 = raws.iter().map(|r| r.len() as u64).sum();
    assert_eq!(u64_at(&cp, 32), raws.len() as u64 + p + s);
    assert_eq!(u64_at(&cp, 40), l + 28 * p + 32 * r + 40 * s + 8 * e);
}

#[test]
fn repeated_mixed_advances_and_empty_identity_are_canonical_at_every_cut() {
    let f = fixture(); full_source(&f); let last = f.chunks.len() - 1; let bytes = f.bytes();
    for stride in [1, 2, 5, last] {
        let mut cp = direct(&f, 0); let mut first = 0;
        while first < last {
            assert_eq!(combined::advance_combined(&cp, &[], Limits::default()).unwrap(), cp);
            let end = (first + stride).min(last);
            cp = combined::advance_combined(&cp, &bytes[offset(&f, first)..offset(&f, end)], Limits::default()).unwrap();
            assert_eq!(cp, direct(&f, end), "noncanonical cut {first}..{end}"); first = end;
        }
    }
}

#[test]
fn late_owner_in_repaired_checkpoint_is_rejected_despite_final_declaration_table() {
    let f = fixture(); full_source(&f); let mut cp = direct(&f, f.chunks.len() - 1);
    let raw = meta(&cp, T_LANES, 0);
    cp[raw.start + 32..raw.start + 36].copy_from_slice(&2u32.to_le_bytes()); repair_raw(&mut cp, raw);
    // Owner 2 is declared later inside the first SPU event block; a final
    // dictionary would wrongly accept this historical reference.
    malformed(read(&cp, &[], Limits::default()), Rule::Extension);
}

#[test]
fn actual_prefix_owner_use_before_declaration_is_rejected() {
    let mut f = fixture(); full_source(&f); let at = f.find(T_THREADS); let declaration = f.chunks.remove(at);
    let after = f.find(T_LANES) + 1; f.chunks.insert(after, declaration);
    assert_eq!(reader::read(&f.bytes()).unwrap_err().rule, Rule::Extension);
    malformed(combined::checkpoint_combined(&f.bytes()[..offset(&f, f.chunks.len() - 1)], Limits::default()), Rule::Extension);
}

#[test]
fn profile_is_required_at_original_two_including_after_early_checkpoint() {
    let f = fixture(); full_source(&f); let cp = direct(&f, 2);
    let mut missing = f.clone(); missing.chunks.remove(2);
    malformed(read(&cp, &missing.bytes()[offset(&missing, 2)..], Limits::default()), Rule::ProfileMissing);
    let mut moved = f.clone(); moved.chunks.swap(2, 3);
    malformed(combined::checkpoint_combined(&moved.bytes()[..offset(&moved, 4)], Limits::default()), Rule::ProfileMissing);
    let mut duplicate = f.clone(); duplicate.chunks.insert(3, duplicate.chunks[2].clone());
    malformed(combined::checkpoint_combined(&duplicate.bytes()[..offset(&duplicate, 4)], Limits::default()), Rule::ProfilePlacement);
}

#[test]
fn imported_profile_uses_actual_clock_frequency_and_flags() {
    let f = fixture(); full_source(&f); let cp = direct(&f, f.chunks.len() - 1);
    let mut bad = cp.clone(); let raw = meta(&bad, T_CLOCKS, 0);
    put64(&mut bad, raw.start + 28, FREQ + 1); repair_raw(&mut bad, raw);
    malformed(read(&bad, &[], Limits::default()), Rule::Profile);
    let mut bad = cp; bad[28..32].copy_from_slice(&1u32.to_le_bytes()); repair(&mut bad);
    reject(read(&bad, &[], Limits::default()));
}

#[test]
fn repaired_outer_frontier_and_family_chunk_count_forgery_are_rejected() {
    let f = fixture(); full_source(&f); let cp = direct(&f, f.chunks.len() - 1);
    for field in [32, 40] {
        let mut bad = cp.clone(); put64(&mut bad, field, u64_at(&cp, field) + 1); repair(&mut bad);
        reject(read(&bad, &[], Limits::default()));
    }
    let mut bad = cp.clone(); let row = sections(&bad)[1].1.start;
    put64(&mut bad, row + 120, 1); repair(&mut bad); reject(read(&bad, &[], Limits::default()));
    let mut bad = cp; let block = blocks(&bad)[0].start;
    put64(&mut bad, block + 32 + 24, 1); repair(&mut bad); reject(read(&bad, &[], Limits::default()));
}

#[test]
fn malformed_schema_support_references_and_overlapping_metadata_are_rejected() {
    let f = fixture(); full_source(&f); let cp = direct(&f, f.chunks.len() - 1);
    let mut bad = cp.clone(); bad[64] = 2; repair(&mut bad); reject(read(&bad, &[], Limits::default()));
    let mut bad = cp.clone(); bad[72] = 1; repair(&mut bad); reject(read(&bad, &[], Limits::default()));
    let mut bad = cp.clone(); let entry = sections(&bad)[3].1.start;
    assert_eq!(u32_at(&bad, entry), 1);
    bad[entry + 24..entry + 28].copy_from_slice(&0u32.to_le_bytes()); repair(&mut bad);
    reject(read(&bad, &[], Limits::default()));
    let mut bad = cp; let raw = meta(&bad, T_THREADS, 1); let first = blocks(&bad)[0].start;
    let endpoint = u64_at(&bad, first + 8); put64(&mut bad, raw.start + 8, endpoint); repair_raw(&mut bad, raw);
    reject(read(&bad, &[], Limits::default()));
}

fn lifetime_fixture() -> File {
    let a = vec![frame(0, 1000, 0)]; let b = vec![frame(0, 1100, 0)];
    let mut old = lane(1, a.clone()); old.state = 1;
    let mut f = scenario(vec![old.clone(), lane(2, b.clone())]).file(); let trailer = f.chunks.pop().unwrap();
    f.chunks.truncate(3); f.chunks.insert(2, chunk(T_PROFILE, Prof::default().payload())); f.flags = 7;
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.ext.push(chunk(T_THREADS, threads_payload(&[thr(1), thr(2)])));
    c.events(1, &[(10000, D_WORK, BEGIN, 0)]);
    c.ext.push(chunk(T_RECORDS, records_payload(1, &a)));
    c.events(1, &[(9990, D_WORK, END, 0)]);
    c.ext.push(chunk(T_RETIRED, retired_payload(&[old.fin()])));
    c.ext.push(chunk(T_RECORDS, records_payload(2, &b)));
    c.drained(1, 1200); c.finish(1);
    f.chunks.extend(c.ext); f.chunks.push(trailer); f
}

#[test]
fn disjoint_lifetime_demand_cannot_borrow_slots_outside_retirement_even_with_exact_totals() {
    let f = lifetime_fixture(); full_source(&f); let mut cp = direct(&f, f.chunks.len() - 1);
    let block = blocks(&cp)[0].start; let first = u64_at(&cp, block + 8); let last = u64_at(&cp, block + 16);
    assert_eq!(last, first + 2); // PPU's actual chunk occupies the interior.
    let retired = meta(&cp, T_RETIRED, 0); assert_eq!(u64_at(&cp, retired.start + 8), last + 1);
    // Force SPU onto the only two slots of old PPU's shortened lifetime. The
    // now-free later slot can serve the other lane, but cannot serve this one.
    put64(&mut cp, block + 16, first + 1); put64(&mut cp, retired.start + 8, last);
    repair_raw(&mut cp, retired);
    reject(read(&cp, &[], Limits::default()));
}

fn reused_slot_fixture() -> File {
    let rows = vec![begin(0, 0, 0, 1000, 1), bparent(1, 0, 0), end(2, 0, 0, 1010, 1)];
    let mut old = lane(1, rows.clone()); old.state = 1;
    let mut f = scenario(vec![old.clone(), lane(2, rows.clone())]).file(); let trailer = f.chunks.pop().unwrap();
    f.chunks.truncate(3); f.chunks.insert(2, chunk(T_PROFILE, Prof::default().payload())); f.flags = 7;
    let mut one = thr(1); one.slot = 1; let mut two = thr(2); two.slot = 1;
    f.chunks.extend([chunk(T_THREADS, threads_payload(&[one])), chunk(T_RECORDS, records_payload(1, &rows)),
        chunk(T_RETIRED, retired_payload(&[old.fin()]))]);
    let mut c = Cap::new(1, 1000); owner(&mut c, 1); c.epoch(1, 0, 1001);
    c.events(1, &[(10000, D_WORK, BEGIN, 0), (9990, D_WORK, END, 0)]); c.drained(1, 1200); c.finish(1);
    f.chunks.extend(c.ext);
    f.chunks.extend([chunk(T_THREADS, threads_payload(&[two])), chunk(T_RECORDS, records_payload(2, &rows)), trailer]); f
}

#[test]
fn retired_owner_stays_declared_and_reused_slot_has_separate_record_demand() {
    let f = reused_slot_fixture(); full_source(&f); let bytes = f.bytes();
    for cut in 0..f.chunks.len() {
        let cp = direct(&f, cut); let got = read(&cp, &bytes[offset(&f, cut)..], Limits::default()).unwrap();
        assert!(got.ppu.incomplete.is_empty());
        assert_eq!(got.ppu.lanes.len(), 2);
        let advanced = combined::advance_combined(&cp, &bytes[offset(&f, cut)..offset(&f, f.chunks.len() - 1)], Limits::default()).unwrap();
        assert_eq!(advanced, direct(&f, f.chunks.len() - 1));
    }
}

fn repeated_fixture(pairs: u64) -> (File, usize) {
    let mut records = Vec::new(); let mut c = Cap::new(1, 1000); owner(&mut c, 1); c.epoch(1, 0, 1001);
    for n in 0..pairs {
        let rows = vec![begin(3 * n, 0, 0, 1100 + n * 20, n + 1), bparent(3 * n + 1, 0, 0), end(3 * n + 2, 0, 0, 1110 + n * 20, n + 1)];
        records.extend_from_slice(&rows); c.ext.push(chunk(T_RECORDS, records_payload(1, &rows)));
        c.events(1, &[(100000 - n as u32 * 20, D_WORK, BEGIN, 0), (99990 - n as u32 * 20, D_WORK, END, 0)]);
    }
    let before = c.ext.len(); c.drained(1, 100000); c.finish(1);
    let f = assemble(scenario(vec![lane(1, records)]).file(), c);
    (f, 5 + before)
}

#[test]
fn alternating_closed_event_chunks_do_not_create_position_history_growth() {
    let (small, a) = repeated_fixture(1); let (large, b) = repeated_fixture(256);
    full_source(&small); full_source(&large);
    let limits = Limits { position_work: 1024, ..Limits::default() };
    let one = combined::checkpoint_combined(&small.bytes()[..offset(&small, a)], limits).unwrap();
    let many = combined::checkpoint_combined(&large.bytes()[..offset(&large, b)], limits).unwrap();
    assert_eq!(one.len(), many.len(), "fixed identities/support cannot grow an event-position ledger");
    assert_eq!(blocks(&many).len(), 1); assert!(many.len() < 16384);
    let got = read(&many, &large.bytes()[offset(&large, b)..], limits).unwrap();
    assert_eq!(got.ppu.lanes[&1].prefix_written, 768);
    assert_eq!(got.spu.retained.sources[&SourceKey::Lane(1)].prefix_events, 512);
    assert!(got.spu.causal.lanes[&1].endpoints.is_empty());
}

#[test]
fn malformed_framing_torn_prefix_and_terminal_successor_are_distinct_failures() {
    let f = fixture(); full_source(&f); let bytes = f.bytes(); let cp = direct(&f, 2);
    reject(combined::checkpoint_combined(&bytes[..offset(&f, 3) - 1], Limits::default()));
    reject(combined::checkpoint_combined(&bytes, Limits::default()));
    reject(combined::advance_combined(&cp, &bytes[offset(&f, 2)..], Limits::default()));
    let mut bad = f.clone(); bad.bad_crc = Some(2);
    malformed(read(&cp, &bad.bytes()[offset(&bad, 2)..], Limits::default()), Rule::ChunkCrc);
    let mut bad = f.clone(); bad.seq_override = Some((2, 8));
    malformed(read(&cp, &bad.bytes()[offset(&bad, 2)..], Limits::default()), Rule::ChunkSeqOrder);
    let last = f.chunks.len() - 1; let cp = direct(&f, last);
    let got = read(&cp, &bytes[offset(&f, last)..bytes.len() - 1], Limits::default()).unwrap();
    assert!(got.torn_tail && !got.terminal); assert_eq!(got.next_chunk, last as u64);
    let mut trailing = bytes[offset(&f, last)..].to_vec(); trailing.push(0);
    malformed(read(&cp, &trailing, Limits::default()), Rule::TrailingBytesAfterTrailer);
}

#[test]
fn cumulative_metadata_and_joint_event_work_are_not_reset_per_family() {
    let f = fixture(); full_source(&f); let last = f.chunks.len() - 1; let cp = direct(&f, last);
    let metadata_bytes = 16 + f.chunks[..last].iter().filter(|c| !matches!(c.ty, T_RECORDS | T_SPU))
        .map(|c| 4 + 20 + c.payload.len()).sum::<usize>();
    limit(read(&cp, &f.bytes()[offset(&f, last)..], Limits { metadata_bytes, ..Limits::default() }), "metadata bytes");
    // Each family individually fits twelve rows, but the actual mixed prefix
    // has twelve PPU rows plus six SPU events.
    limit(combined::checkpoint_combined(&f.bytes()[..offset(&f, last)], Limits { work_events: 12, ..Limits::default() }), "event work");
    limit(combined::checkpoint_combined(&f.bytes()[..offset(&f, 2)], Limits { input_bytes: 0, ..Limits::default() }), "input bytes");
    limit(read(&cp, &[], Limits { checkpoint_bytes: 0, ..Limits::default() }), "checkpoint bytes");
}

#[test]
fn malformed_prefix_event_shape_does_not_escape_a_zero_work_budget() {
    let mut f = fixture(); full_source(&f); let records = f.find(T_RECORDS);
    let mut empty = Buf::default(); empty.u32(1).u32(0);
    f.chunks[records] = chunk(T_RECORDS, empty.0);
    malformed(combined::checkpoint_combined(&f.bytes()[..offset(&f, records + 1)],
        Limits { work_events: 0, ..Limits::default() }), Rule::RecordsCountZero);
}

#[test]
fn unsupported_flags_and_gpu_domain_never_enter_combined_state() {
    let mut f = fixture(); full_source(&f); f.flags |= 0x8000_0000;
    match combined::checkpoint_combined(&f.bytes()[..16], Limits::default()) {
        Err(Error::Unsupported { .. }) => {}, Err(e) => panic!("expected unsupported flags, got {e:?}"), Ok(_) => panic!("accepted flags"),
    }
    let mut f = fixture(); let domain = f.find(T_DOMAIN); f.chunks[domain].payload[8] = 2;
    match combined::checkpoint_combined(&f.bytes()[..offset(&f, domain + 1)], Limits::default()) {
        Err(Error::Unsupported { .. }) => {}, Err(e) => panic!("expected unsupported GPU domain, got {e:?}"), Ok(_) => panic!("accepted GPU"),
    }
}

fn fresh_frame_membership_fixture() -> File {
    let mut frames = vec![frame(0, 1000, 0)];
    for n in 1..4096u64 {
        // Genuine producer loss leaves 4096 distinct exact membership runs.
        frames.extend([gap(2 * n - 1, 2 * n - 1), frame(2 * n, 1000 + n, 2 * n)]);
    }
    let mut spans = Vec::new();
    for n in 0..4096u64 {
        spans.extend([begin(3 * n, 0, 0, 10000 + 2 * n, n + 1),
            bparent_tok(3 * n + 1, 0, 0, 1, 1), end(3 * n + 2, 0, 0, 10001 + 2 * n, n + 1)]);
    }
    let mut f = scenario(vec![lane(1, frames), lane(2, spans)]).file();
    // EXT with no domains is valid. This isolates PPU membership admission
    // inside the combined API; there are no SPU ordering/clock complications.
    f.flags = 1;
    let a = match load(&f.bytes()) {
        Outcome::Analysed(a) => a, Outcome::Malformed(m) => panic!("invalid resource fixture: {m}"),
    };
    assert!(a.capture.incomplete.is_empty());
    assert_eq!(a.capture.lanes[&1].frames.len(), 4096);
    assert_eq!(a.capture.lanes[&1].gap_count, 4095);
    assert_eq!(a.capture.spans.len(), 4096);
    f
}

#[test]
fn snapshot_charges_fresh_frame_membership_before_token_scan() {
    let f = fresh_frame_membership_fixture();
    // 20,479 input physical rows fit the current event charge, but checking
    // 4096 missing token references against 4096 fresh frame runs does not.
    limit(combined::checkpoint_combined(&f.bytes()[..offset(&f, f.chunks.len() - 1)],
        Limits { work_items: 8_000_000, ..Limits::default() }), "structural work");
}

#[test]
fn retained_report_charges_fresh_frame_membership_before_token_scan() {
    let f = fresh_frame_membership_fixture(); let cut = f.find(T_RECORDS);
    let cp = direct(&f, cut);
    limit(read(&cp, &f.bytes()[offset(&f, cut)..],
        Limits { work_items: 8_000_000, ..Limits::default() }), "structural work");
}

#[test]
fn larger_membership_budget_accepts_the_same_valid_source() {
    let f = fresh_frame_membership_fixture();
    let limits = Limits { work_items: 64_000_000, ..Limits::default() };
    let encoded = combined::checkpoint_combined(&f.bytes()[..offset(&f, f.chunks.len() - 1)], limits).unwrap();
    assert_eq!(u64_at(&encoded, 32), (f.chunks.len() - 1) as u64);
    let cut = f.find(T_RECORDS); let cp = direct(&f, cut);
    let got = read(&cp, &f.bytes()[offset(&f, cut)..], limits).unwrap();
    assert!(got.terminal && got.ppu.incomplete.is_empty());
    assert_eq!(got.ppu.lanes[&1].retained_written, 4096);
    assert_eq!(got.ppu.lanes[&2].retained_written, 12288);
    assert_eq!(got.ppu.zones.iter().find(|z| z.name == "zoneA").unwrap().completed, 4096);
}
