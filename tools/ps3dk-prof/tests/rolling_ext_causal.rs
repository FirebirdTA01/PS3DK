//! Retained causal-support controls against a full-source comparison.
//! The comparison parses the entire independently written original source. It
//! never runs the legacy tick lift on a compacted retained capture.
#[allow(dead_code)]
mod common;
mod spu_support;
use common::*;
use spu_support::*;
use std::collections::{BTreeMap, BTreeSet};
use ps3dk_prof::{Analysis, Outcome};
use ps3dk_prof::ext::{SourceKey, V1Refs};
use ps3dk_prof::ext::rolling::{Adapter, Limits};
use ps3dk_prof::ext::rolling::causal::{Bounds, CausalLimits, CausalOptions, CounterHistory, Finished, Placement,
    PlacementBasis, Refusal, CAUSAL_ONLY_NOTE};
use ps3dk_prof::spu_ticks::{self, ObsKind, Options, SpuResult};

fn fixture() -> Cap {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.bracket(1, 10000, 1100, 1110, Some(0));
    c.events(1, &[(9990, D_WORK, BEGIN, 0)]);
    c.events(1, &[(9980, D_PING, BEGIN, 1), (9970, D_PING, END, 1), (9960, D_WORK, END, 0)]);
    c.bracket(1, 9950, 1200, 1210, Some(4)); c.drained(1, 1250);
    c.events(1, &[(9940, D_OTHER, BEGIN, 0), (9930, D_OTHER, END, 0)]);
    c.bracket(1, 9920, 1300, 1310, Some(6)); c.drained(1, 1350); c.finish(1); c
}
fn full_source(c: &Cap) -> (Box<Analysis>, SpuResult) {
    let a = match load(&c.bytes()) { Outcome::Analysed(a) => a, Outcome::Malformed(m) => panic!("invalid original fixture: {m}") };
    let result = spu_ticks::analyze(&a, &Options { table: test_table(), ..Options::default() }).unwrap();
    (a, result)
}
fn nth(c: &Cap, ty: u32, n: usize) -> usize {
    c.ext.iter().enumerate().filter(|(_, c)| c.ty == ty).nth(n).unwrap().0
}
fn order(c: &Cap, kind: u32, n: usize) -> usize {
    c.ext.iter().enumerate().filter(|(_, c)| c.ty == T_ORDER
        && u32::from_le_bytes(c.payload[8..12].try_into().unwrap()) == kind).nth(n).unwrap().0
}
fn run(c: &Cap, cut: usize, reviewed: bool, limits: CausalLimits) -> Finished {
    let (a, expected) = full_source(c);
    let ids: BTreeSet<_> = a.capture.lanes.keys().copied().collect();
    let refs = V1Refs { descriptors: &a.capture.descriptors, declared_incarnations: &ids };
    let frames = a.capture.lanes.iter().map(|(&id, l)| (id, l.frames.iter().map(|f| f.id).collect())).collect();
    let base = c.file().chunks.iter().position(|x| matches!(x.ty, 8..=16 | 18)).unwrap() as u64;
    let mut parser = Adapter::new(c.flags, Limits::default()).unwrap();
    for (i, ch) in c.ext[..cut].iter().enumerate() { parser.accept(base + i as u64, ch.ty, &ch.payload, &refs).unwrap(); }
    let cp = parser.checkpoint().unwrap();
    let mut parser = Adapter::from_checkpoint(&cp, &refs, Limits::default()).unwrap();
    for i in cut..c.ext.len() { parser.accept(base + i as u64, c.ext[i].ty, &c.ext[i].payload, &refs).unwrap(); }
    parser.finish_with_causal(true, true, &frames, CausalOptions {
        classification: reviewed.then_some(&expected.classification), limits,
    }).unwrap()
}
fn full_bounds(c: &Cap) -> BTreeMap<(u32, u64), Bounds> {
    let (_, full) = full_source(c);
    full.lanes.iter().flat_map(|lane| lane.obs.iter().filter_map(move |o| match o.kind {
        ObsKind::Event { seq, .. } => Some(((lane.lane_id, seq), Bounds {
            lower: o.l, upper_drain: o.u1, upper_bracket: o.u2, upper: o.u,
        })), _ => None,
    })).collect()
}
fn reasons(p: &Placement) -> &[Refusal] { match p { Placement::Unavailable(r) => r, _ => &[] } }

#[test]
fn every_original_support_cut_matches_full_source_causal_endpoints() {
    let c = fixture(); let expected = full_bounds(&c);
    for cut in 0..=c.ext.len() {
        let got = run(&c, cut, true, CausalLimits::default());
        assert!(got.causal.refusal.is_none());
        assert_eq!(got.causal.basis, PlacementBasis::CausalSupportOnly);
        assert_eq!(got.causal.counter_history, CounterHistory::NotEvaluated);
        assert_eq!(got.causal.note, CAUSAL_ONLY_NOTE);
        let events = &got.retained.sources[&SourceKey::Lane(1)].events;
        let rows = &got.causal.lanes[&1].endpoints;
        assert_eq!(rows.iter().map(|r| r.seq).collect::<Vec<_>>(), events.iter().map(|e| e.seq).collect::<Vec<_>>());
        for row in rows {
            assert_eq!(row.bounds, expected[&(1, row.seq)], "cut {cut}, endpoint {}", row.seq);
            assert!(matches!(row.placement, Placement::QualifiedCausal { .. }));
        }
    }
}

#[test]
fn crossing_begin_is_not_restored_as_an_endpoint_or_complete_span() {
    let c = fixture(); let cut = nth(&c, T_SPU, 0) + 1;
    let got = run(&c, cut, true, CausalLimits::default());
    assert!(got.causal.lanes[&1].endpoints.iter().all(|e| e.seq != 0));
    let source = &got.retained.sources[&SourceKey::Lane(1)];
    assert_eq!(source.left_censored.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(0, 3)]);
    assert_eq!(source.pairs.iter().map(|p| (p.begin, p.end)).collect::<Vec<_>>(), vec![(1, 2), (4, 5)]);
}

#[test]
fn historical_drain_tb_decrease_refuses_a_later_clean_window() {
    let mut c = Cap::new(1, 900); c.epoch(1, 0, 901);
    c.events(1, &[(10000, D_WORK, BEGIN, 0), (9990, D_WORK, END, 0)]); c.drained(1, 1200);
    c.events(1, &[(9980, D_WORK, BEGIN, 0), (9970, D_WORK, END, 0)]); c.drained(1, 1100);
    let cut = c.ext.len();
    c.events(1, &[(9960, D_WORK, BEGIN, 0), (9950, D_WORK, END, 0)]);
    c.bracket(1, 9940, 1300, 1310, Some(6)); c.drained(1, 1400); c.finish(1);
    let got = run(&c, cut, true, CausalLimits::default());
    assert_eq!(got.causal.lanes[&1].order.first_tb_decrease, Some((1200, 1100, 4)));
    for row in &got.causal.lanes[&1].endpoints { assert!(reasons(&row.placement).contains(&Refusal::DrainTbDecrease)); }
}

#[test]
fn evicted_causal_contradiction_survives_without_any_drain_tb_decrease() {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.bracket(1, 10000, 2000, 2010, Some(0));
    c.events(1, &[(9990, D_WORK, BEGIN, 0), (9980, D_WORK, END, 0)]); c.drained(1, 1500);
    let cut = c.ext.len();
    c.events(1, &[(9970, D_WORK, BEGIN, 0), (9960, D_WORK, END, 0)]);
    c.bracket(1, 9950, 3000, 3010, Some(4)); c.drained(1, 3500); c.finish(1);
    let (_, full) = full_source(&c); assert!(full.lanes[0].order_contradicted > 0);
    let got = run(&c, cut, true, CausalLimits { finding_rows: 0, ..CausalLimits::default() });
    let lane = &got.causal.lanes[&1];
    assert!(lane.order.first_tb_decrease.is_none());
    assert!(lane.order.contradicted_epochs.contains(&1));
    assert!(lane.order.witnesses.is_empty(), "display witness cap must not disable contradiction detection");
    for row in &lane.endpoints { assert!(reasons(&row.placement).contains(&Refusal::CausalOrderContradiction { epoch: 1 })); }
}

#[test]
fn no_order_dropped_order_and_unpositioned_brackets_have_distinct_reasons() {
    for mode in 0..3 {
        let mut c = fixture();
        if mode == 0 { c.flags = 3; c.ext.retain(|ch| ch.ty != T_ORDER); }
        if mode == 1 { let end = order(&c, 5, 0); c.ext[end].payload[24..32].copy_from_slice(&2u64.to_le_bytes()); }
        if mode == 2 {
            let position = order(&c, 1, 0); c.ext.remove(position);
            let end = order(&c, 5, 0); let n = u64::from_le_bytes(c.ext[end].payload[16..24].try_into().unwrap());
            c.ext[end].payload[16..24].copy_from_slice(&(n - 1).to_le_bytes());
        }
        let got = run(&c, nth(&c, T_SPU, 0), true, CausalLimits::default());
        for row in &got.causal.lanes[&1].endpoints {
            let r = reasons(&row.placement);
            match mode {
                0 => assert!(r.contains(&Refusal::NoOrder)),
                1 => assert!(r.contains(&Refusal::OrderDropped(2))),
                _ => assert!(r.contains(&Refusal::UnpositionedBrackets { epoch: 1, count: 1 })),
            }
        }
    }
}

#[test]
fn prefix_loss_never_becomes_eviction_only_loss_or_a_valid_source() {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.events(1, &[(10000, D_WORK, BEGIN, 0), (9990, D_WORK, END, 0)]); c.loss(1, 2); c.drained(1, 1200);
    let cut = c.ext.len(); c.epoch(1, 0, 1201);
    c.events(1, &[(9980, D_WORK, BEGIN, 0), (9970, D_WORK, END, 0)]);
    c.bracket(1, 9960, 1300, 1310, Some(6)); c.drained(1, 1400); c.finish(1);
    let expected = full_bounds(&c); let got = run(&c, cut, true, CausalLimits::default());
    for row in &got.causal.lanes[&1].endpoints {
        assert_eq!(row.bounds, expected[&(1, row.seq)]);
        assert!(row.seq >= 4);
        assert!(reasons(&row.placement).contains(&Refusal::SourceLoss { prefix: 2, retained: 0 }));
    }
}

#[test]
fn absent_or_unreviewed_profile_classification_is_conditional_not_reviewed() {
    for classification_present in [false, true] {
        let mut c = fixture();
        if classification_present { c.profile.as_mut().unwrap().id = b"UNREVIEWED".to_vec(); }
        let got = run(&c, nth(&c, T_SPU, 0), classification_present, CausalLimits::default());
        if classification_present { assert!(!got.causal.classification.as_ref().unwrap().holds()); }
        else { assert!(got.causal.classification.is_none()); }
        for row in &got.causal.lanes[&1].endpoints { assert!(matches!(row.placement, Placement::ConditionalCausal { .. })); }
    }
}

#[test]
fn causal_work_scales_with_compact_support_not_evicted_event_count() {
    let build = |n: u32| {
        let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
        for i in 0..n { c.events(1, &[(100000 - 2*i, D_WORK, BEGIN, 0), (99999 - 2*i, D_WORK, END, 0)]); }
        let cut = c.ext.len(); c.events(1, &[(99000, D_OTHER, BEGIN, 0), (98990, D_OTHER, END, 0)]);
        c.bracket(1, 98980, 1300, 1310, Some(u64::from(2*n + 2))); c.drained(1, 1400); c.finish(1); (c, cut)
    };
    let (a, acut) = build(1); let (b, bcut) = build(256);
    let a = run(&a, acut, true, CausalLimits { work_items: 1024, ..CausalLimits::default() });
    let b = run(&b, bcut, true, CausalLimits { work_items: 1024, ..CausalLimits::default() });
    assert!(a.causal.refusal.is_none() && b.causal.refusal.is_none());
    assert_eq!(a.causal.work_items, b.causal.work_items);
    assert_eq!(b.causal.lanes[&1].endpoints.len(), 2);
}

#[test]
fn causal_resource_refusal_is_atomic_and_keeps_structural_validation() {
    let c = fixture(); let cut = nth(&c, T_SPU, 0) + 1;
    for limits in [CausalLimits { work_items: 0, ..CausalLimits::default() },
        CausalLimits { groups: 0, ..CausalLimits::default() }, CausalLimits { endpoint_rows: 0, ..CausalLimits::default() }] {
        let got = run(&c, cut, true, limits);
        assert!(matches!(got.causal.refusal, Some(Refusal::Resource(_))));
        assert!(got.causal.lanes.is_empty());
        let source = &got.retained.sources[&SourceKey::Lane(1)];
        assert_eq!((source.prefix_events, source.retained_events), (1, 5));
        assert!(source.raw_valid);
    }
}

#[test]
fn missing_final_missing_bounds_and_empty_intervals_are_explicitly_refused() {
    for mode in 0..4 {
        let mut c = Cap::new(1, if mode == 3 { 2000 } else { 1000 }); c.epoch(1, 0, 1001);
        c.events(1, &[(10000, D_WORK, BEGIN, 0), (9990, D_WORK, END, 0)]);
        if mode != 2 { c.drained(1, if mode == 3 { 1000 } else { 1200 }); }
        c.finish(1);
        if mode == 0 { c.ext.retain(|ch| ch.ty != T_FINAL && !(ch.ty == T_ORDER
            && u32::from_le_bytes(ch.payload[8..12].try_into().unwrap()) == 5)); }
        if mode == 1 {
            let created = order(&c, 4, 0); c.ext.remove(created);
            let end = order(&c, 5, 0); let n = u64::from_le_bytes(c.ext[end].payload[16..24].try_into().unwrap());
            c.ext[end].payload[16..24].copy_from_slice(&(n - 1).to_le_bytes());
        }
        let got = run(&c, nth(&c, T_SPU, 0), true, CausalLimits::default());
        for row in &got.causal.lanes[&1].endpoints {
            let expected = match mode { 0 => Refusal::NoOrderFinal, 1 => Refusal::MissingLower,
                2 => Refusal::MissingUpper, _ => Refusal::EmptyInterval };
            assert!(reasons(&row.placement).contains(&expected));
            if mode == 3 { assert_eq!((row.bounds.lower, row.bounds.upper), (Some(2000), Some(1000))); }
        }
    }
}

#[test]
fn causal_lower_bound_carries_across_a_known_empty_epoch_transition() {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.bracket(1, 10000, 1900, 1910, Some(0));
    c.events(1, &[(9990, D_WORK, BEGIN, 0), (9980, D_WORK, END, 0)]); c.drained(1, 2000);
    c.epoch(1, 0, 2100); let cut = c.ext.len();
    c.events(1, &[(9970, D_WORK, BEGIN, 0), (9960, D_WORK, END, 0)]);
    c.bracket(1, 9950, 2200, 2210, Some(4)); c.drained(1, 2300); c.finish(1);
    let expected = full_bounds(&c); let got = run(&c, cut, true, CausalLimits::default());
    for row in &got.causal.lanes[&1].endpoints {
        assert_eq!(row.epoch, 2); assert_eq!(row.bounds.lower, Some(1900));
        assert_eq!(row.bounds, expected[&(1, row.seq)]);
        assert!(matches!(row.placement, Placement::QualifiedCausal { .. }));
    }
}

#[test]
fn historical_protocol_error_remains_a_placement_refusal() {
    let mut c = Cap::new(1, 1000); c.epoch(1, 0, 1001);
    c.events(1, &[(10000, D_WORK, BEGIN, 1), (9990, D_WORK, END, 1)]); c.drained(1, 1200);
    let cut = c.ext.len(); c.events(1, &[(9980, D_WORK, BEGIN, 0), (9970, D_WORK, END, 0)]);
    c.bracket(1, 9960, 1300, 1310, Some(4)); c.drained(1, 1400); c.finish(1);
    let got = run(&c, cut, true, CausalLimits::default());
    assert!(!got.retained.sources[&SourceKey::Lane(1)].raw_valid);
    for row in &got.causal.lanes[&1].endpoints { assert!(reasons(&row.placement).contains(&Refusal::ProtocolError)); }
}
