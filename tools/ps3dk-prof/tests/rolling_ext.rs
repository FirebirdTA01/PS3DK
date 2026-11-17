//! Independent full-source fixtures and standalone SPU checkpoint transitions.
#[allow(dead_code)]
mod common;
mod spu_support;
use std::collections::{BTreeMap, BTreeSet};
use common::*;
use spu_support::*;
use ps3dk_prof::ext::{self, OpenBegins, SourceKey, V1Refs};
use ps3dk_prof::ext::rolling::{Adapter, Error, Limits, Retained};
use ps3dk_prof::model::{Capture, Rule};

fn fixture() -> Cap {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.bracket(1, 10000, 1100, 1110, Some(0));
    c.events(1, &[(9990, D_WORK, BEGIN, 0)]);
    c.events(1, &[(9980, D_PING, BEGIN, 1), (9970, D_PING, END, 1), (9960, D_WORK, END, 0)]);
    c.bracket(1, 9950, 1200, 1210, Some(4)); c.drained(1, 1250);
    c.events(1, &[(9940, D_OTHER, BEGIN, 0), (9930, D_OTHER, END, 0)]);
    c.bracket(1, 9920, 1300, 1310, Some(6)); c.drained(1, 1350); c.finish(1); c
}
fn context(c: &Cap) -> Capture {
    let full = reader::read(&c.bytes()).expect("independently framed full source must be structurally valid");
    assert!(full.incomplete.is_empty()); full
}
fn declared(cap: &Capture) -> BTreeSet<u32> { cap.lanes.keys().copied().collect() }
fn frames(cap: &Capture) -> BTreeMap<u32, BTreeSet<u64>> {
    cap.lanes.iter().map(|(&id, l)| (id, l.frames.iter().map(|f| f.id).collect())).collect()
}
fn original_base(c: &Cap) -> u64 {
    c.file().chunks.iter().position(|x| matches!(x.ty, 8..=16 | 18)).unwrap() as u64
}
fn feed(a: &mut Adapter, c: &Cap, first: usize, end: usize, cap: &Capture) -> Result<(), Error> {
    let ids = declared(cap); let refs = V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids };
    let base = original_base(c);
    for i in first..end { a.accept(base + i as u64, c.ext[i].ty, &c.ext[i].payload, &refs)?; }
    Ok(())
}
fn checkpoint(c: &Cap, at: usize, limits: Limits) -> Result<Vec<u8>, Error> {
    let cap = context(c); let mut a = Adapter::new(c.flags, limits)?;
    feed(&mut a, c, 0, at, &cap)?; a.checkpoint()
}
fn restored(c: &Cap, at: usize) -> (Adapter, Capture) {
    let cp = checkpoint(c, at, Limits::default()).expect("SPU typed checkpoint implemented");
    let cap = context(c); let ids = declared(&cap);
    let a = Adapter::from_checkpoint(&cp, &V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids }, Limits::default())
        .expect("SPU typed state imports"); (a, cap)
}
fn suffix(c: &Cap, at: usize) -> Retained {
    let (mut a, cap) = restored(c, at); feed(&mut a, c, at, c.ext.len(), &cap).unwrap();
    a.finish(true, true, &frames(&cap)).unwrap()
}
fn nth(c: &Cap, ty: u32, n: usize) -> usize { c.ext.iter().enumerate().filter(|(_, c)| c.ty == ty).nth(n).unwrap().0 }
fn order(c: &Cap, kind: u32, n: usize) -> usize {
    c.ext.iter().enumerate().filter(|(_, c)| c.ty == T_ORDER
        && u32::from_le_bytes(c.payload[8..12].try_into().unwrap()) == kind).nth(n).unwrap().0
}
fn expect_ext(result: Result<(), Error>) {
    assert!(matches!(result, Err(Error::Malformed(m)) if m.rule == Rule::Extension), "shared extension validator must reject");
}

#[test]
fn crossing_begin_is_support_while_retained_interior_pairs_remain_complete() {
    let c = fixture(); let at = nth(&c, T_SPU, 0) + 1; let got = suffix(&c, at);
    let s = &got.sources[&SourceKey::Lane(1)];
    assert_eq!((s.prefix_events, s.retained_events, s.prefix_lost, s.retained_lost), (1, 5, 0, 0));
    assert_eq!(s.events.iter().map(|e| e.seq).collect::<Vec<_>>(), vec![1, 2, 3, 4, 5]);
    assert_eq!(s.pairs.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(1, 2), (4, 5)]);
    assert_eq!(s.left_censored.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(0, 3)]);
    assert!(s.raw_valid); assert_eq!(s.open_begins, OpenBegins::Verified(0));
}

#[test]
fn calibration_and_order_support_survive_every_original_chunk_boundary() {
    let c = fixture(); let cap = context(&c); let full = cap.ext.as_ref().unwrap();
    for at in 0..=c.ext.len() {
        let got = suffix(&c, at);
        let s = &got.sources[&SourceKey::Lane(1)];
        assert_eq!(s.prefix_events + s.retained_events, 6);
        assert_eq!(s.events.len() as u64, s.retained_events);
        assert_eq!(got.brackets, full.brackets);
        assert_eq!(got.order, full.order);
        assert_eq!(got.epochs, full.epochs);
    }
}

#[test]
fn order_cursor_epoch_and_atomic_drain_pair_rules_survive_restart() {
    let c = fixture();
    for (at, offset, value) in [
        (order(&c, 1, 0), 24usize, 1u64), // BRACKET_AT before > cursor zero
        (order(&c, 2, 0), 24usize, 5u64), // DRAINED through disagrees with paired DRAIN_TB
        (order(&c, 2, 0), 16usize, 2u64), // DRAINED wrong current epoch
    ] {
        let (mut a, cap) = restored(&c, at); let mut bad = c.clone();
        bad.ext[at].payload[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
        expect_ext(feed(&mut a, &bad, at, at + 1, &cap));
    }
    let at = order(&c, 2, 0); let (mut a, cap) = restored(&c, at); let mut bad = c;
    bad.ext[at] = order_chunk(&[rec_drained(1, 1, 4)]);
    expect_ext(feed(&mut a, &bad, at, at + 1, &cap));
}

#[test]
fn rejected_chunk_poisons_checkpoint_and_finish_publication() {
    let c = fixture(); let at = order(&c, 2, 0);
    let (mut a, cap) = restored(&c, at); let mut bad = c;
    // Keep the DRAINED/DRAIN_TB pair internally matched, but move both beyond
    // the source cursor. The existing parser charges global order-record work
    // before discovering this cursor error: publishing that state is unsafe.
    bad.ext[at].payload[24..32].copy_from_slice(&5u64.to_le_bytes());
    bad.ext[at].payload[48..56].copy_from_slice(&5u64.to_le_bytes());
    expect_ext(feed(&mut a, &bad, at, at + 1, &cap));
    assert!(a.checkpoint().is_err(), "a rejected chunk must prevent checkpoint publication");
    assert!(a.finish(true, true, &frames(&cap)).is_err(), "a rejected chunk must prevent final-result publication");
}

#[test]
fn restored_original_positions_reject_duplicates_and_backwards_chunks() {
    let c = fixture(); let at = nth(&c, T_SPU, 0) + 1;
    for index in [original_base(&c) + at as u64 - 1, original_base(&c)] {
        let (mut a, cap) = restored(&c, at); let ids = declared(&cap);
        let refs = V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids };
        assert!(matches!(a.accept(index, c.ext[at].ty, &c.ext[at].payload, &refs), Err(Error::Checkpoint(_))));
        assert!(a.checkpoint().is_err());
    }
}

#[test]
fn source_limit_is_admitted_before_parsing_or_allocating_declaration_rows() {
    let c = fixture(); let cap = context(&c); let ids = declared(&cap);
    let refs = V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids };
    let mut a = Adapter::new(7, Limits { sources: 0, ..Limits::default() }).unwrap();
    // This header already exceeds policy. The intentionally absent row would
    // otherwise fail payload parsing; no row allocation is needed to refuse.
    assert!(matches!(a.accept(3, T_DOMAIN, &1u32.to_le_bytes(), &refs), Err(Error::Limit(_))));
    assert!(a.checkpoint().is_err());
}

#[test]
fn known_depth_zero_loss_and_open_epoch_transitions_keep_original_final_semantics() {
    for mode in 0..5 {
        let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
        if mode == 2 { c.events(1, &[(10000, D_WORK, BEGIN, 0)]); }
        else { c.events(1, &[(10000, D_WORK, BEGIN, 0), (9990, D_WORK, END, 0)]); }
        if mode == 1 { c.loss(1, 2); }
        c.drained(1, 1100); let at = c.ext.len(); c.epoch(1, 0, 1101);
        if mode == 3 { c.ext.last_mut().unwrap().payload[12] = 5; c.lane(1).unknown = true; }
        c.events(1, &[(9980, D_OTHER, BEGIN, 0), (9970, D_OTHER, END, 0)]); c.drained(1, 1200); c.finish(1);
        if mode == 4 { c.ext.last_mut().unwrap().payload[36..40].copy_from_slice(&u32::MAX.to_le_bytes()); }
        let cap = context(&c); let original = &cap.ext.as_ref().unwrap().sources[0]; let got = suffix(&c, at);
        let s = &got.sources[&SourceKey::Lane(1)];
        assert_eq!(s.open_begins, original.open_begins, "epoch mode {mode}");
        assert_eq!(s.raw_valid, original.raw_valid, "epoch mode {mode}");
        assert_eq!(s.prefix_lost + s.retained_lost, original.lost_total);
        assert_eq!(s.pairs.len(), 1); assert!(s.left_censored.is_empty());
    }
}

#[test]
fn source_and_order_final_counts_include_prefix_without_replaying_events() {
    let c = fixture();
    for field in [12usize, 20, 28, 36] {
        let at = nth(&c, T_FINAL, 0); let (mut a, cap) = restored(&c, at); let mut bad = c.clone();
        if field == 36 { bad.ext[at].payload[36..40].copy_from_slice(&1u32.to_le_bytes()); }
        else { let old = u64::from_le_bytes(bad.ext[at].payload[field..field + 8].try_into().unwrap());
            bad.ext[at].payload[field..field + 8].copy_from_slice(&(old + 1).to_le_bytes()); }
        expect_ext(feed(&mut a, &bad, at, at + 1, &cap));
    }
    let at = order(&c, 5, 0); let (mut a, cap) = restored(&c, at); let mut bad = c;
    bad.ext[at].payload[16..24].copy_from_slice(&999u64.to_le_bytes());
    expect_ext(feed(&mut a, &bad, at, at + 1, &cap));
}

#[test]
fn finalized_lane_does_not_reappear_when_thread_is_reused_under_new_identity() {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.events(1, &[(10000, D_WORK, BEGIN, 0), (9990, D_WORK, END, 0)]); c.drained(1, 1100); c.finish(1);
    let at = c.ext.len(); c.declare(&[2], 1200);
    let lane = nth(&c, T_LANES, 1); c.ext[lane].payload[20..28].copy_from_slice(&1u64.to_le_bytes());
    c.epoch(2, 0, 1201); c.events(2, &[(9980, D_WORK, BEGIN, 0), (9970, D_WORK, END, 0)]);
    c.drained(2, 1300); c.finish(2);
    let got = suffix(&c, at);
    assert_eq!((got.sources[&SourceKey::Lane(1)].prefix_events, got.sources[&SourceKey::Lane(1)].retained_events), (2, 0));
    assert!(got.sources[&SourceKey::Lane(1)].pairs.is_empty());
    assert_eq!(got.sources[&SourceKey::Lane(2)].pairs.len(), 1);
    let (mut a, cap) = restored(&c, lane); let mut bad = c;
    bad.ext[lane].payload[4..8].copy_from_slice(&1u32.to_le_bytes());
    expect_ext(feed(&mut a, &bad, lane, lane + 1, &cap));
}

#[test]
fn closed_event_runs_compact_across_many_chunks_between_support_boundaries() {
    let build = |n: u32| {
        let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
        for i in 0..n { c.events(1, &[(100000 - 2*i, D_WORK, BEGIN, 0), (99999 - 2*i, D_WORK, END, 0)]); }
        let at = c.ext.len(); c.drained(1, 2000); c.finish(1); (c, at)
    };
    let (small, a) = build(1); let (large, b) = build(256);
    let limits = Limits { event_runs: 1, ..Limits::default() };
    let first = checkpoint(&small, a, limits).unwrap(); let many = checkpoint(&large, b, limits).unwrap();
    assert_eq!(first.len(), many.len(), "neither closed rows nor per-event-chunk headers belong in the checkpoint");
    assert!(many.len() < 8192);
    let got = suffix(&large, b); assert!(got.sources[&SourceKey::Lane(1)].events.is_empty());
}

#[test]
fn historical_protocol_error_and_drain_tb_decrease_are_sticky() {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.events(1, &[(10000, D_WORK, BEGIN, 1), (9990, D_WORK, END, 1)]); c.drained(1, 1200);
    let at = c.ext.len(); c.events(1, &[(9980, D_WORK, BEGIN, 0), (9970, D_WORK, END, 0)]);
    c.drained(1, 1100); c.finish(1); let full = context(&c); let got = suffix(&c, at);
    let s = &got.sources[&SourceKey::Lane(1)]; assert!(!s.raw_valid); assert!(s.protocol_error.is_some());
    assert!(s.pairs.is_empty());
    assert_eq!(got.order.as_ref().unwrap().lanes[&1].tb_decrease, full.ext.as_ref().unwrap().order.as_ref().unwrap().lanes[&1].tb_decrease);
}

#[test]
fn missing_final_remains_incomplete_and_resource_caps_refuse() {
    let c = fixture(); let at = nth(&c, T_SPU, 0) + 1;
    let (a, cap) = restored(&c, at); let got = a.finish(false, false, &frames(&cap)).unwrap();
    let s = &got.sources[&SourceKey::Lane(1)]; assert_eq!(s.structure, ext::Structure::Incomplete);
    assert_eq!(s.open_begins, OpenBegins::NoFinal); assert_eq!(s.open_prefix, vec![0]);
    assert!(!s.raw_valid);
    let cp = checkpoint(&c, at, Limits::default()).unwrap();
    for limits in [Limits { checkpoint_bytes: cp.len() - 1, ..Limits::default() },
        Limits { metadata_bytes: 1, ..Limits::default() }, Limits { sources: 0, ..Limits::default() },
        Limits { event_runs: 0, ..Limits::default() }, Limits { stack_rows: 0, ..Limits::default() },
        Limits { work_events: 0, ..Limits::default() }] {
        assert!(matches!(checkpoint(&c, at, limits), Err(Error::Limit(_))));
    }
}

#[test]
fn unsupported_families_and_corrupt_checkpoint_are_not_partial_successes() {
    assert!(matches!(Adapter::new(7 | 128, Limits::default()), Err(Error::Unsupported(_))));
    let c = fixture(); let at = nth(&c, T_SPU, 0); let (mut a, cap) = restored(&c, at); let ids = declared(&cap);
    let refs = V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids };
    assert!(matches!(a.accept(original_base(&c) + at as u64, 14, &[], &refs), Err(Error::Unsupported(_))));
    let mut cp = checkpoint(&c, at, Limits::default()).unwrap(); cp[0] ^= 1;
    assert!(matches!(Adapter::from_checkpoint(&cp, &refs, Limits::default()), Err(Error::Checkpoint(_))));
}

#[test]
fn checkpoint_version_and_source_capabilities_remain_bound_after_crc_repair() {
    let c = fixture(); let at = nth(&c, T_SPU, 0); let cp = checkpoint(&c, at, Limits::default()).unwrap();
    assert_eq!(&cp[..8], b"PFRSPU01");
    let cap = context(&c); let ids = declared(&cap);
    let refs = V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids };
    let repair = |bytes: &mut Vec<u8>| { let end = bytes.len() - 4; let crc = crc32(&bytes[..end]);
        bytes[end..].copy_from_slice(&crc.to_le_bytes()); };
    let mut version = cp.clone(); version[8..10].copy_from_slice(&2u16.to_le_bytes()); repair(&mut version);
    assert!(matches!(Adapter::from_checkpoint(&version, &refs, Limits::default()), Err(Error::Checkpoint(_))));
    let mut unknown = cp.clone(); unknown[12..16].copy_from_slice(&(7u32 | 128).to_le_bytes()); repair(&mut unknown);
    assert!(matches!(Adapter::from_checkpoint(&unknown, &refs, Limits::default()), Err(Error::Unsupported(_))));
    // Removing the ORDER capability cannot turn already decoded order support
    // into an ignored family, even if all integrity checks are repaired.
    let mut missing = cp; missing[12..16].copy_from_slice(&3u32.to_le_bytes()); repair(&mut missing);
    assert!(Adapter::from_checkpoint(&missing, &refs, Limits::default()).is_err());
}

#[test]
fn one_chunk_event_block_cannot_claim_two_different_endpoint_positions() {
    let c = fixture(); let at = nth(&c, T_SPU, 0) + 1;
    let mut cp = checkpoint(&c, at, Limits::default()).unwrap();
    let mut entry = 64;
    while u32::from_le_bytes(cp[entry..entry + 4].try_into().unwrap()) != 2 {
        entry += u32::from_le_bytes(cp[entry + 4..entry + 8].try_into().unwrap()) as usize;
    }
    let first = u64::from_le_bytes(cp[entry + 8..entry + 16].try_into().unwrap());
    let widened = first + 1;
    cp[entry + 16..entry + 24].copy_from_slice(&widened.to_le_bytes());
    cp[32..40].copy_from_slice(&widened.to_le_bytes());
    let end = cp.len() - 4; let crc = crc32(&cp[..end]); cp[end..].copy_from_slice(&crc.to_le_bytes());
    let cap = context(&c); let ids = declared(&cap);
    let refs = V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids };
    assert!(matches!(Adapter::from_checkpoint(&cp, &refs, Limits::default()), Err(Error::Checkpoint(_))),
        "one original chunk cannot simultaneously occupy two distinct endpoint positions");
}

#[test]
fn independent_lane_interleavings_have_one_canonical_block_summary() {
    let build = |order: &[u32]| {
        let mut c = Cap::new(2, 1000); c.epoch(1, 0, 1001); c.epoch(2, 0, 1002);
        for &lane in order {
            let first = c.lane(lane).events == 0;
            c.events(lane, &[(10000 * lane - if first { 0 } else { 10 }, D_WORK, if first { BEGIN } else { END }, 0)]);
        }
        let at = c.ext.len();
        c.drained(1, 1100); c.finish(1); c.drained(2, 1200); c.finish(2); (c, at)
    };
    let (a, at) = build(&[1, 2, 1, 2]); let (b, bt) = build(&[2, 1, 2, 1]);
    let limits = Limits { event_runs: 2, ..Limits::default() };
    let acp = checkpoint(&a, at, limits).unwrap(); let bcp = checkpoint(&b, bt, limits).unwrap();
    assert_eq!(acp, bcp, "within-block cross-lane order is intentionally abstracted; actual support boundaries remain exact");
    for (c, cut) in [(a, at), (b, bt)] {
        let got = suffix(&c, cut);
        assert_eq!(got.sources.len(), 2);
        assert!(got.sources.values().all(|s| s.prefix_events == 2 && s.retained_events == 0 && s.events.is_empty()));
    }
}

#[test]
fn imported_event_block_cannot_end_at_unassignable_original_index() {
    let c = fixture(); let at = nth(&c, T_SPU, 1) + 1;
    let mut cp = checkpoint(&c, at, Limits::default()).unwrap();
    let mut entry = 64;
    while u32::from_le_bytes(cp[entry..entry + 4].try_into().unwrap()) != 2 {
        entry += u32::from_le_bytes(cp[entry + 4..entry + 8].try_into().unwrap()) as usize;
    }
    // Two original chunks can occupy distinct endpoints, but no accepted
    // source chunk may use u64::MAX. Import must enforce the same bound.
    cp[entry + 16..entry + 24].copy_from_slice(&u64::MAX.to_le_bytes());
    cp[32..40].copy_from_slice(&u64::MAX.to_le_bytes());
    let end = cp.len() - 4; let crc = crc32(&cp[..end]); cp[end..].copy_from_slice(&crc.to_le_bytes());
    let cap = context(&c); let ids = declared(&cap);
    let refs = V1Refs { descriptors: &cap.descriptors, declared_incarnations: &ids };
    assert!(matches!(Adapter::from_checkpoint(&cp, &refs, Limits::default()), Err(Error::Checkpoint(_))));
}
