//! Section 10, "Stage 2, reader": one test per listed fixture, named after its bullet.  The "every MALFORMED rule"
//! bullet is tests/malformed.rs.  tests/SPEC-COVERAGE.md maps each bullet to its test.

mod common;

use common::*;
use ps3dk_prof::{
    compare, compare_with, load, reader, report, AbResult, Attribution, CompareOptions, ComparisonKind, Incomplete,
    Metric, Outcome, Rule, SpansN, TokenOutcome,
};

/// One completed zone on lane 1.
fn one_zone() -> Vec<Rec> {
    seqd(0, [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)]].concat())
}

fn lane_reasons(a: &ps3dk_prof::Analysis, inc: u32) -> Vec<String> {
    a.lanes[&inc].reasons.clone()
}

fn span_of(a: &ps3dk_prof::Analysis, inc: u32, span_id: u64) -> usize {
    a.capture
        .spans
        .iter()
        .position(|s| s.incarnation == inc && s.span_id == span_id)
        .expect("span present")
}

fn token_outcome(a: &ps3dk_prof::Analysis) -> TokenOutcome {
    assert_eq!(a.tokens.len(), 1);
    a.tokens[0].outcome
}

/// Truncate a file inside its last chunk (the TRAILER, for scenario files).
fn torn(bytes: Vec<u8>) -> Vec<u8> {
    let n = bytes.len() - 7;
    bytes[..n].to_vec()
}

// ---- base list --------------------------------------------------------------------------------------------------

#[test]
fn torn_tail_gives_incomplete() {
    let (_, why) = incomplete(&torn(scenario(vec![lane(1, one_zone())]).bytes()));
    assert!(why.contains(&Incomplete::TornTail));
    assert!(why.contains(&Incomplete::NoTrailer));
}

#[test]
fn begin_parent_split_across_chunks() {
    let recs = one_zone();
    let mut f = scenario(vec![lane(1, recs.clone())]).file();
    let i = f.find(T_RECORDS);
    f.chunks[i] = chunk(T_RECORDS, records_payload(1, &recs[..1]));
    f.chunks.insert(i + 1, chunk(T_RECORDS, records_payload(1, &recs[1..])));
    assert_rule(&f.bytes(), Rule::BeginParentPlacement);
}

#[test]
fn adjacent_gap_pair() {
    let recs = vec![frame(0, 10, 0), gap(1, 1), gap(2, 2), frame(3, 40, 3)];
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::GapPlacement);
}

#[test]
fn gap_at_end_of_a_chunk() {
    // The GAP closes chunk 1 and its successor opens chunk 2: still MALFORMED.
    let recs = vec![frame(0, 10, 0), gap(1, 1), frame(2, 30, 2)];
    let mut f = scenario(vec![lane(1, recs.clone())]).file();
    let i = f.find(T_RECORDS);
    f.chunks[i] = chunk(T_RECORDS, records_payload(1, &recs[..2]));
    f.chunks.insert(i + 1, chunk(T_RECORDS, records_payload(1, &recs[2..])));
    assert_rule(&f.bytes(), Rule::GapPlacement);
}

#[test]
fn produced_count_2_pow_63_accepted() {
    // GAP [0, 2^63 - 2] then one FRAME at 2^63 - 1: produced_count = 2^63.
    let l = lane(1, vec![gap(0, P63 - 2), frame(P63 - 1, 10, 0)]);
    assert_eq!(l.fin().produced, P63);
    let a = complete(&scenario(vec![l]).bytes());
    assert_eq!(a.capture.lanes[&1].final_entry.as_ref().unwrap().produced_count, P63);
}

#[test]
fn produced_count_2_pow_63_plus_1_refused() {
    let mut l = lane(1, vec![gap(0, P63 - 2), frame(P63 - 1, 10, 0)]);
    l.terminal = 1;
    assert_eq!(l.fin().produced, P63 + 1);
    assert_rule(&scenario(vec![l]).bytes(), Rule::ProducedCountRange);
}

#[test]
fn two_slot_begin_at_2_pow_63_minus_1_refused() {
    // The last written seq is 2^63 - 2; the BEGIN that would take 2^63 - 1 was refused whole.
    let mut l = lane(1, vec![gap(0, P63 - 3), frame(P63 - 2, 10, 0)]);
    l.refused = 1;
    l.limit = true;
    let (a, why) = incomplete(&scenario(vec![l]).bytes());
    assert_eq!(why, vec![Incomplete::SeqLimit]);
    assert_eq!(a.capture.lanes[&1].final_entry.as_ref().unwrap().produced_count, P63 - 1);
    let r = lane_reasons(&a, 1);
    assert!(r.iter().any(|x| x == "SEQ_LIMIT") && r.iter().any(|x| x == "refused 1"), "{r:?}");
}

#[test]
fn regerr_dropped_while_regerr_count_positive_is_invalid_not_clean() {
    // The REGERR event was dropped (terminal loss); regerr_count still says 1.
    let mut l = lane(1, one_zone());
    l.terminal = 1;
    l.regerr = 1;
    let a = complete(&scenario(vec![l]).bytes());
    assert!(!a.lane_valid(1));
    let r = lane_reasons(&a, 1);
    assert!(r.iter().any(|x| x == "regerr 1") && r.iter().any(|x| x == "dropped 1"), "{r:?}");
    assert!(a.zones.is_empty(), "an invalid lane contributes no statistics");
}

#[test]
fn consistency_arithmetic_terminal_tail_1_with_written_0() {
    // Lost first-and-only record: produced 1, written 0, dropped 1.  Terminal loss, never "empty".
    let mut l = lane(1, Vec::new());
    l.terminal = 1;
    let f = l.fin();
    assert_eq!((f.produced, f.written, f.dropped), (1, 0, 1));
    let a = complete(&scenario(vec![l]).bytes());
    assert!(!a.lane_valid(1));
    assert!(a.lanes[&1].loss);
}

#[test]
fn consistency_arithmetic_empty_lane_is_valid() {
    let a = complete(&scenario(vec![lane(1, Vec::new())]).bytes());
    assert!(a.lane_valid(1));
}

#[test]
fn retired_then_records() {
    let mut l = lane(1, one_zone());
    l.state = 1;
    let mut f = scenario(vec![l]).file();
    let i = f.find(T_RETIRED);
    f.chunks.insert(i + 1, chunk(T_RECORDS, records_payload(1, &[frame(3, 300, 0)])));
    assert_rule(&f.bytes(), Rule::RecordsAfterRetired);
}

#[test]
fn decreasing_tb_gives_clock_anomaly() {
    let recs = seqd(0, [zone(0, 0, 0, 200, 1, 0), vec![end(0, 0, 0, 150, 1)]].concat());
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert!(!a.lane_valid(1));
    assert!(lane_reasons(&a, 1).iter().any(|x| x == "CLOCK_ANOMALY"));
}

#[test]
fn frame_id_hole_without_loss() {
    let recs = vec![frame(0, 10, 0), frame(1, 20, 2)];
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::FrameIdSequence);
}

#[test]
fn frame_id_hole_with_loss() {
    // FRAME 1 was lost in the GAP.
    let recs = vec![frame(0, 10, 0), gap(1, 1), frame(2, 30, 2)];
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert!(!a.lane_valid(1));
}

// ---- ancestry under loss (r2) -----------------------------------------------------------------------------------

#[test]
fn parent_begin_dropped_then_child_begin_child_end_parent_end() {
    // Parent span 10 (BEGIN+BEGIN_PARENT, seq 0-1) was lost; child 11 at depth 1, then both ENDs.
    let recs = seqd(
        0,
        [vec![gap(0, 1)], zone(2, 1, 1, 150, 11, 10), vec![end(4, 1, 1, 180, 11), end(5, 0, 0, 200, 10)]].concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert!(!a.lane_valid(1));
    let child = &a.capture.spans[span_of(&a, 1, 11)];
    assert!(child.affected);
    assert_eq!(child.end_tb, Some(180));
    assert_eq!(a.capture.lanes[&1].affected_ends, 1, "the parent's END is tolerated as AFFECTED");
}

#[test]
fn partial_stack_with_unknown_base_wrong_child_depth() {
    let recs = seqd(0, [vec![gap(0, 1)], zone(2, 0, 3, 100, 1, 99), zone(4, 1, 3, 150, 2, 1)].concat());
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::StackBeginMismatch);
}

#[test]
fn partial_stack_with_unknown_base_wrong_parent() {
    let recs = seqd(0, [vec![gap(0, 1)], zone(2, 0, 3, 100, 1, 99), zone(4, 1, 4, 150, 2, 7)].concat());
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::StackBeginMismatch);
}

#[test]
fn depth_0_resync_then_wrong_parent_is_malformed() {
    let recs = seqd(0, [vec![gap(0, 1)], zone(2, 0, 0, 100, 1, 0), zone(4, 1, 1, 150, 2, 9)].concat());
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::StackBeginMismatch);
}

#[test]
fn depth_0_resync_makes_base_known() {
    // After the resync an END on an empty stack is MALFORMED again.
    let recs = seqd(
        0,
        [vec![gap(0, 1)], zone(2, 0, 0, 100, 1, 0), vec![end(4, 0, 0, 150, 1), end(5, 0, 0, 160, 3)]].concat(),
    );
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::KnownEmptyEnd);
}

#[test]
fn depth_0_begin_while_partial_stack_non_empty() {
    let recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), zone(2, 1, 0, 150, 2, 0)].concat());
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::StackBeginMismatch);
}

#[test]
fn depth_parent_zero_mismatch_base_known() {
    let recs = seqd(0, [zone(0, 0, 0, 100, 1, 5), vec![end(2, 0, 0, 200, 1)]].concat());
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::DepthParentZeroMismatch);
}

#[test]
fn depth_parent_zero_mismatch_base_unknown() {
    let recs = seqd(0, [vec![gap(0, 1)], zone(2, 0, 2, 100, 1, 0)].concat());
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::DepthParentZeroMismatch);
}

#[test]
fn end_on_empty_stack_base_known_is_malformed() {
    let recs = vec![end(0, 0, 0, 100, 1)];
    assert_rule(&scenario(vec![lane(1, recs)]).bytes(), Rule::KnownEmptyEnd);
}

#[test]
fn end_on_empty_stack_base_unknown_is_affected() {
    let recs = seqd(0, vec![gap(0, 1), end(2, 0, 0, 100, 1)]);
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert_eq!(a.capture.lanes[&1].affected_ends, 1);
    assert!(!a.lane_valid(1));
}

// ---- final entries (r2) -----------------------------------------------------------------------------------------

#[test]
fn state_3_entry_with_exact_unknown_fields_is_incomplete() {
    let mut l = lane(1, one_zone());
    l.state = 3;
    let (a, why) = incomplete(&scenario(vec![l]).bytes());
    assert_eq!(why, vec![Incomplete::NotQuiesced]);
    assert!(!a.lane_valid(1));
}

#[test]
fn state_3_entry_with_concrete_produced_count() {
    let mut l = lane(1, one_zone());
    l.state = 3;
    let s = scenario(vec![l]);
    let mut t = s.trailer_struct();
    t.fins[0].produced = 3;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    assert_rule(&f.bytes(), Rule::State3Fields);
}

#[test]
fn state_1_entry_containing_unknown() {
    let mut l = lane(1, one_zone());
    l.state = 1;
    let mut fin = l.fin();
    fin.regerr = UNKNOWN;
    let mut f = scenario(vec![l]).file();
    let i = f.find(T_RETIRED);
    f.chunks[i] = chunk(T_RETIRED, retired_payload(&[fin]));
    assert_rule(&f.bytes(), Rule::ExactEntryUnknown);
}

#[test]
fn state_2_entry_containing_unknown() {
    let s = scenario(vec![lane(1, one_zone())]);
    let mut t = s.trailer_struct();
    t.fins[0].open = UNKNOWN;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    assert_rule(&f.bytes(), Rule::ExactEntryUnknown);
}

fn retired_limited_scenario() -> Scenario {
    let mut retired = lane(1, one_zone());
    retired.state = 1;
    retired.limit = true;
    retired.refused = 1;
    scenario(vec![retired, lane(2, Vec::new())])
}

#[test]
fn seq_limit_only_in_earlier_retired_entry_bit1_required() {
    let (_, why) = incomplete(&retired_limited_scenario().bytes());
    assert_eq!(why, vec![Incomplete::SeqLimit]);
}

#[test]
fn seq_limit_only_in_earlier_retired_entry_bit1_missing() {
    let s = retired_limited_scenario();
    let mut t = s.trailer_struct();
    assert_eq!(t.end_flags & 2, 2);
    t.end_flags &= !2;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    assert_rule(&f.bytes(), Rule::EndFlagSeqLimitMismatch);
}

#[test]
fn seq_limit_bit1_with_no_limited_entry() {
    let s = scenario(vec![lane(1, one_zone())]);
    let mut t = s.trailer_struct();
    t.end_flags |= 2;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    assert_rule(&f.bytes(), Rule::EndFlagSeqLimitMismatch);
}

// ---- censoring (r2) ---------------------------------------------------------------------------------------------

/// zoneA: one completed and one TRUNCATED_AT_STOP instance; zoneB: one clean instance.
fn censored_capture() -> Vec<u8> {
    let recs = seqd(
        0,
        [
            zone(0, 0, 0, 100, 1, 0),
            vec![end(0, 0, 0, 200, 1)],
            zone(0, 1, 0, 300, 2, 0),
            vec![end(0, 1, 0, 350, 2)],
            zone(0, 0, 0, 400, 3, 0),
            vec![end_trunc(0, 0, 0, 450, 3)],
        ]
        .concat(),
    );
    scenario(vec![lane(1, recs)]).bytes()
}

fn clean_capture() -> Vec<u8> {
    let recs = seqd(
        0,
        [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 210, 1)], zone(0, 1, 0, 300, 2, 0), vec![end(0, 1, 0, 360, 2)]]
            .concat(),
    );
    scenario(vec![lane(1, recs)]).bytes()
}

#[test]
fn zone_with_one_truncated_instance_is_labelled_and_ab_refused_censored() {
    let a = complete(&censored_capture());
    let za = a.zone("zoneA").expect("zoneA");
    assert_eq!((za.instances, za.completed, za.censored), (2, 1, 1));
    assert_eq!(za.label.as_deref(), Some("completed instances only: 1 censored of 2"));
    assert!(a.lane_valid(1), "censoring alone does not invalidate the lane");
    let b = complete(&clean_capture());
    let rows = compare(&a, &b).rows;
    let row = rows.iter().find(|r| r.name == "zoneA").unwrap();
    match &row.result {
        AbResult::Refused(why) => assert!(why.starts_with("CENSORED (A: 1 censored of 2, B: 0 censored of 1)"), "{why}"),
        other => panic!("expected CENSORED refusal, got {other:?}"),
    }
}

#[test]
fn clean_zone_in_the_same_capture_compares_normally() {
    let a = complete(&censored_capture());
    let b = complete(&clean_capture());
    let rows = compare(&a, &b).rows;
    let row = rows.iter().find(|r| r.name == "zoneB").unwrap();
    assert_eq!(row.result, AbResult::Compared { a_mean: 50.0, b_mean: 60.0 });
    assert!(a.zone("zoneB").unwrap().label.is_none());
}

#[test]
fn unclosed_last_frame_is_censored() {
    let recs = vec![frame(0, 100, 0), frame(1, 250, 1)];
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert_eq!(a.elapsed_for(1, 0), Some(&Metric::Value(150)));
    assert_eq!(a.elapsed_for(1, 1), Some(&Metric::Unknown("censored: unclosed frame".to_string())));
}

// ---- frames and tokens (r3) -------------------------------------------------------------------------------------

fn owner_frames() -> Vec<Rec> {
    vec![frame(0, 100, 0), frame(1, 300, 1)]
}

fn worker_tok(frame_id: u64) -> Vec<Rec> {
    seqd(0, [zone_tok(0, 1, 0, 120, 1, 0, 1, frame_id), vec![end(0, 1, 0, 170, 1)]].concat())
}

#[test]
fn future_arriving_valid_frame_resolves() {
    // The worker's RECORDS chunk is written before the owner's FRAME chunk.
    let a = complete(&scenario(vec![lane(2, worker_tok(0)), lane(1, owner_frames())]).bytes());
    assert!(a.capture.lanes.keys().copied().eq([1, 2]));
    assert_eq!(token_outcome(&a), TokenOutcome::Resolved);
    let s = span_of(&a, 2, 1);
    assert_eq!(a.spans[s].attribution, Attribution::Frame { owner: 1, frame: 0 });
}

#[test]
fn nonexistent_frame_complete_lossless_quiesced_owner_is_malformed() {
    let b = scenario(vec![lane(1, owner_frames()), lane(2, worker_tok(5))]).bytes();
    assert_rule(&b, Rule::TokenImpossible);
}

#[test]
fn nonexistent_frame_with_owner_loss_is_unresolved_loss() {
    let owner = lane(1, vec![frame(0, 100, 0), gap(1, 1), frame(2, 300, 2)]);
    let a = complete(&scenario(vec![owner, lane(2, worker_tok(1))]).bytes());
    assert_eq!(token_outcome(&a), TokenOutcome::UnresolvedLoss);
    // The owner's frame elapsed and every W for that owner are refused.
    assert!(a.frames.iter().filter(|f| f.owner == 1).all(|f| f.value.is_refused()));
    assert!(a.work.iter().filter(|w| w.owner == 1).all(|w| w.value.is_refused()));
    assert!(!a.work.is_empty());
    // The worker's zone statistics are still reported.
    assert!(a.lane_valid(2));
    assert_eq!(a.zone("zoneB").unwrap().completed, 1);
}

#[test]
fn clean_worker_with_invalid_owner() {
    let mut owner = lane(1, owner_frames());
    owner.regerr = 1;
    let a = complete(&scenario(vec![owner, lane(2, worker_tok(0))]).bytes());
    assert!(!a.lane_valid(1) && a.lane_valid(2));
    let s = span_of(&a, 2, 1);
    assert_eq!(a.spans[s].attribution, Attribution::OwnerInvalid { owner: 1, frame: 0 });
    assert!(a.spans[s].attribution.to_string().contains("owner invalid"));
    assert_eq!(a.work_for(1, 0, 2), Some(&Metric::Refused("owner invalid".to_string())));
    assert_eq!(a.zone("zoneB").unwrap().completed, 1);
}

#[test]
fn owner_untokened_span_before_first_frame_is_pre_frame() {
    let recs = seqd(0, [zone(0, 0, 0, 50, 1, 0), vec![end(0, 0, 0, 60, 1), frame(0, 100, 0)]].concat());
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert_eq!(a.spans[span_of(&a, 1, 1)].attribution, Attribution::PreFrame);
    assert_eq!(a.spans[span_of(&a, 1, 1)].attribution.to_string(), "UNKNOWN(pre-frame)");
}

#[test]
fn owner_untokened_span_after_gap_before_next_frame_is_loss() {
    let recs = seqd(
        0,
        [vec![frame(0, 100, 0), gap(1, 1)], zone(2, 0, 0, 150, 1, 0), vec![end(4, 0, 0, 160, 1), frame(5, 300, 2)]]
            .concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert_eq!(a.spans[span_of(&a, 1, 1)].attribution, Attribution::Loss);
    assert_eq!(a.spans[span_of(&a, 1, 1)].attribution.to_string(), "UNKNOWN(loss)");
}

// ---- token resolution, one fixture per rule and per INCOMPLETE reason (r4) --------------------------------------

/// Owner 1 (frames 0 and 1, lossless, ACKED) and worker 2 whose token names `frame_id`.
fn token_scenario(frame_id: u64) -> Scenario {
    scenario(vec![lane(1, owner_frames()), lane(2, worker_tok(frame_id))])
}

#[test]
fn rule1_present_frame_in_no_trailer_capture_resolves() {
    let (a, why) = incomplete(&token_scenario(0).file().without_trailer().bytes());
    assert!(why.contains(&Incomplete::NoTrailer));
    assert_eq!(token_outcome(&a), TokenOutcome::Resolved);
}

#[test]
fn rule2_missing_no_trailer() {
    let (a, _) = incomplete(&token_scenario(5).file().without_trailer().bytes());
    assert_eq!(token_outcome(&a), TokenOutcome::UnresolvedIncomplete);
}

#[test]
fn rule2_missing_torn_tail() {
    let (a, why) = incomplete(&torn(token_scenario(5).bytes()));
    assert!(why.contains(&Incomplete::TornTail));
    assert_eq!(token_outcome(&a), TokenOutcome::UnresolvedIncomplete);
}

#[test]
fn rule2_missing_owner_has_no_final_entry() {
    let s = token_scenario(5);
    let mut t = s.trailer_struct();
    t.fins.retain(|f| f.inc != 1);
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    let (a, why) = incomplete(&f.bytes());
    assert_eq!(why, vec![Incomplete::MissingFinalEntry(1)]);
    assert_eq!(token_outcome(&a), TokenOutcome::UnresolvedIncomplete);
}

#[test]
fn rule3_missing_owner_has_gap() {
    let owner = lane(1, vec![frame(0, 100, 0), gap(1, 1), frame(2, 300, 2)]);
    let a = complete(&scenario(vec![owner, lane(2, worker_tok(1))]).bytes());
    assert_eq!(token_outcome(&a), TokenOutcome::UnresolvedLoss);
}

#[test]
fn rule3_missing_owner_has_terminal_loss_only() {
    let mut owner = lane(1, owner_frames());
    owner.terminal = 1; // FRAME 2 was the dropped tail
    let a = complete(&scenario(vec![owner, lane(2, worker_tok(2))]).bytes());
    assert_eq!(a.capture.lanes[&1].gap_count, 0);
    assert_eq!(token_outcome(&a), TokenOutcome::UnresolvedLoss);
}

#[test]
fn rule3_missing_owner_is_state_3() {
    let mut owner = lane(1, owner_frames());
    owner.state = 3;
    let (a, why) = incomplete(&scenario(vec![owner, lane(2, worker_tok(5))]).bytes());
    assert_eq!(why, vec![Incomplete::NotQuiesced]);
    assert_eq!(token_outcome(&a), TokenOutcome::UnresolvedLoss);
}

#[test]
fn rule4_missing_incomplete_only_through_seq_limit_on_another_lane() {
    let mut other = lane(3, Vec::new());
    other.limit = true;
    other.refused = 1;
    let b = scenario(vec![lane(1, owner_frames()), lane(2, worker_tok(5)), other]).bytes();
    assert_rule(&b, Rule::TokenImpossible);
}

#[test]
fn rule4_missing_incomplete_only_through_seq_limit_on_owner() {
    let mut owner = lane(1, owner_frames());
    owner.limit = true;
    owner.refused = 1;
    assert_rule(&scenario(vec![owner, lane(2, worker_tok(5))]).bytes(), Rule::TokenImpossible);
}

#[test]
fn rule4_missing_incomplete_only_through_recovered_sink_error() {
    let mut s = token_scenario(5);
    s.sink_errors = 1;
    assert_rule(&s.bytes(), Rule::TokenImpossible);
}

#[test]
fn rule4_missing_incomplete_only_through_other_lane_not_quiesced() {
    let mut other = lane(3, Vec::new());
    other.state = 3;
    let b = scenario(vec![lane(1, owner_frames()), lane(2, worker_tok(5)), other]).bytes();
    assert_rule(&b, Rule::TokenImpossible);
}

#[test]
fn rule4_missing_complete_lossless_capture() {
    assert_rule(&token_scenario(5).bytes(), Rule::TokenImpossible);
}

// ---- attributed work (r4) ---------------------------------------------------------------------------------------

#[test]
fn nested_parent_and_child_exclusive_sum_to_parent_inclusive() {
    // FRAME 0; parent 100..300; child 150..250.
    let recs = seqd(
        0,
        [
            vec![frame(0, 0, 0)],
            zone(0, 0, 0, 100, 1, 0),
            zone(0, 1, 1, 150, 2, 1),
            vec![end(0, 1, 1, 250, 2), end(0, 0, 0, 300, 1)],
        ]
        .concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    let p = &a.capture.spans[span_of(&a, 1, 1)];
    let c = &a.capture.spans[span_of(&a, 1, 2)];
    assert_eq!((p.inclusive(), p.exclusive()), (Some(200), Some(100)));
    assert_eq!(c.exclusive(), Some(100));
    assert_eq!(p.exclusive().unwrap() + c.exclusive().unwrap(), p.inclusive().unwrap());
    assert_eq!(a.work_for(1, 0, 1), Some(&Metric::Value(200)), "the parent is not counted twice");
}

#[test]
fn child_in_a_later_frame_than_its_parent() {
    // FRAME 0 @0; parent 100..300; FRAME 1 @150; child 160..260; FRAME 2 @400.
    let recs = seqd(
        0,
        [
            vec![frame(0, 0, 0)],
            zone(0, 0, 0, 100, 1, 0),
            vec![frame(0, 150, 1)],
            zone(0, 1, 1, 160, 2, 1),
            vec![end(0, 1, 1, 260, 2), end(0, 0, 0, 300, 1), frame(0, 400, 2)],
        ]
        .concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert_eq!(a.spans[span_of(&a, 1, 2)].attribution, Attribution::Frame { owner: 1, frame: 1 });
    assert_eq!(a.work_for(1, 1, 1), Some(&Metric::Value(100)), "the child's time is in the child's frame");
    assert_eq!(a.work_for(1, 0, 1), Some(&Metric::Value(100)), "the parent's W excludes the child");
    // W is start-frame attributed and not clipped: frame 0 elapsed is 150, the parent ran 100..300.
    assert_eq!(a.elapsed_for(1, 0), Some(&Metric::Value(150)));
}

#[test]
fn two_overlapping_workers_tokened_to_the_same_frame() {
    let w2 = seqd(0, [zone_tok(0, 1, 0, 120, 1, 0, 1, 0), vec![end(0, 1, 0, 200, 1)]].concat());
    let w3 = seqd(0, [zone_tok(0, 1, 0, 130, 1, 0, 1, 0), vec![end(0, 1, 0, 190, 1)]].concat());
    let a = complete(&scenario(vec![lane(1, owner_frames()), lane(2, w2), lane(3, w3)]).bytes());
    assert_eq!(a.work_for(1, 0, 2), Some(&Metric::Value(80)));
    assert_eq!(a.work_for(1, 0, 3), Some(&Metric::Value(60)));
    let for_frame: Vec<_> = a.work.iter().filter(|w| w.owner == 1 && w.frame == 0).collect();
    assert_eq!(for_frame.len(), 2, "one W per lane, no cross-lane sum");
    let text = report::render(&a);
    assert!(!text.to_lowercase().contains("frame total"), "{text}");
}

#[test]
fn clean_owner_clean_worker_and_invalid_third_lane() {
    let w2 = seqd(0, [zone_tok(0, 1, 0, 120, 1, 0, 1, 0), vec![end(0, 1, 0, 200, 1)]].concat());
    // Lane 3 has a CLOCK_ANOMALY (END before BEGIN in time).
    let w3 = seqd(0, [zone_tok(0, 1, 0, 190, 1, 0, 1, 0), vec![end(0, 1, 0, 130, 1)]].concat());
    let a = complete(&scenario(vec![lane(1, owner_frames()), lane(2, w2), lane(3, w3)]).bytes());
    assert!(a.lane_valid(1) && a.lane_valid(2) && !a.lane_valid(3));
    assert_eq!(a.elapsed_for(1, 0), Some(&Metric::Value(200)));
    assert_eq!(a.work_for(1, 0, 2), Some(&Metric::Value(80)));
    // Lane 3's span ends before it begins, so its duration is unavailable as well as its lane invalid.
    assert_eq!(a.work_for(1, 0, 3), Some(&Metric::Refused("lane invalid, duration unavailable".to_string())));
}

// ---- "spans N frames" (r4) --------------------------------------------------------------------------------------

#[test]
fn owner_span_crossing_two_frames_spans_3() {
    let recs = seqd(
        0,
        [
            vec![frame(0, 0, 0)],
            zone(0, 0, 0, 10, 1, 0),
            vec![frame(0, 20, 1), frame(0, 30, 2), end(0, 0, 0, 40, 1)],
        ]
        .concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    let v = &a.spans[span_of(&a, 1, 1)];
    assert_eq!(v.spans_n, SpansN::Known(3));
    assert_eq!(v.spans_n.to_string(), "spans 3 frames");
}

#[test]
fn tokened_worker_span_n_unknown() {
    let a = complete(&token_scenario(0).bytes());
    let v = &a.spans[span_of(&a, 2, 1)];
    assert_eq!(v.spans_n, SpansN::Unknown);
    assert_eq!(v.spans_n.to_string(), "spans N: unknown");
}

#[test]
fn owner_span_with_gap_inside_n_unknown() {
    // The GAP comes after the BEGIN and before the END: the span is AFFECTED and N is unknown.
    let recs = seqd(
        0,
        [vec![frame(0, 0, 0)], zone(1, 0, 0, 10, 1, 0), vec![gap(3, 3), frame(4, 30, 2)]].concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert_eq!(a.spans[span_of(&a, 1, 1)].spans_n, SpansN::Unknown);
}

// ---- review additions: exact counts at 2^63 are accepted (section 6) -------------------------------------------------
// written_count = 2^63 cannot be built: it must equal the records actually written.

#[test]
fn dropped_count_2_pow_63_accepted() {
    let mut l = lane(1, Vec::new());
    l.terminal = P63;
    assert_eq!((l.fin().produced, l.fin().dropped), (P63, P63));
    let a = complete(&scenario(vec![l]).bytes());
    assert!(!a.lane_valid(1));
}

#[test]
fn refused_count_2_pow_63_accepted() {
    let mut l = lane(1, one_zone());
    l.refused = P63;
    let a = complete(&scenario(vec![l]).bytes());
    assert!(lane_reasons(&a, 1).iter().any(|x| x == &format!("refused {P63}")));
}

#[test]
fn regerr_count_2_pow_63_accepted() {
    let mut l = lane(1, one_zone());
    l.regerr = P63;
    let a = complete(&scenario(vec![l]).bytes());
    assert!(!a.lane_valid(1));
}

#[test]
fn open_spans_256_accepted() {
    // The most spans one stack can hold (depths 0..=255).
    let s = scenario(vec![lane(1, one_zone())]);
    let mut t = s.trailer_struct();
    t.fins[0].open = 256;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    let a = complete(&f.bytes());
    assert_eq!(a.capture.lanes[&1].final_entry.as_ref().unwrap().open_spans, 256);
}

#[test]
fn state3_open_spans_unknown_accepted() {
    let mut l = lane(1, one_zone());
    l.state = 3;
    assert_eq!(l.fin().open, UNKNOWN);
    let (a, why) = incomplete(&scenario(vec![l]).bytes());
    assert_eq!(why, vec![Incomplete::NotQuiesced]);
    assert_eq!(a.capture.lanes[&1].final_entry.as_ref().unwrap().open_spans, UNKNOWN);
}

#[test]
fn regerr_record_with_nonzero_regerr_count_makes_lane_invalid() {
    let mut l = lane(1, vec![regerr(0, 1)]);
    l.regerr = 1;
    let a = complete(&scenario(vec![l]).bytes());
    assert!(!a.lane_valid(1));
    assert!(lane_reasons(&a, 1).iter().any(|x| x == "regerr 1"));
}

// ---- review additions: FRAME ids at the legal maximum (section 4) ----------------------------------------------------

#[test]
fn frame_id_equal_to_its_seq_at_the_maximum_is_accepted() {
    // Every earlier sequence was a dropped FRAME: id = seq = 2^63 - 1 is the largest assignable id.
    let recs = vec![frame(0, 10, 0), gap(1, P63 - 2), frame(P63 - 1, 30, P63 - 1)];
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert_eq!(a.capture.lanes[&1].frames.last().map(|f| f.id), Some(P63 - 1));
}

// ---- review additions: no saturated value (section 9) ----------------------------------------------------------------

#[test]
fn nested_same_zone_spans_of_2_pow_63_have_an_exact_total_and_a_rounded_mean() {
    // zoneA parent 0 .. 2^63 + 2, zoneA child 1 .. 2^63 + 1: durations 2^63 + 2 and 2^63, sum 2^64 + 2.
    let recs = seqd(
        0,
        [
            zone(0, 0, 0, 0, 1, 0),
            zone(0, 0, 1, 1, 2, 1),
            vec![end(0, 0, 1, P63 + 1, 2), end(0, 0, 0, P63 + 2, 1)],
        ]
        .concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    let z = a.zone("zoneA").unwrap();
    let want: u128 = (1u128 << 64) + 2;
    assert_eq!(z.total, Metric::Value(want));
    // The exact mean is 2^63 + 1; the mean is f64, so this compares the already-rounded value, not an exact one.
    assert_eq!(z.mean, Metric::Value(want as f64 / 2.0));
    assert_eq!(a.capture.spans[span_of(&a, 1, 1)].exclusive(), Some(2));
}

#[test]
fn clock_anomaly_span_duration_is_unavailable_not_zero() {
    let recs = seqd(0, [zone(0, 0, 0, 200, 1, 0), vec![end(0, 0, 0, 150, 1)]].concat());
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    let s = &a.capture.spans[span_of(&a, 1, 1)];
    assert_eq!((s.inclusive(), s.exclusive()), (None, None));
    assert!(s.duration_unavailable());
    let text = report::render(&a);
    assert!(text.contains("duration unavailable (CLOCK_ANOMALY: END before BEGIN)"), "{text}");
    assert!(!text.contains("inclusive 0"), "{text}");
}

#[test]
fn parent_of_an_anomalous_child_has_an_unavailable_exclusive_duration() {
    // Parent 100 .. 300; child BEGIN 200, END 150 (an anomaly).
    let recs = seqd(
        0,
        [zone(0, 0, 0, 100, 1, 0), zone(0, 1, 1, 200, 2, 1), vec![end(0, 1, 1, 150, 2), end(0, 0, 0, 300, 1)]].concat(),
    );
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    let p = &a.capture.spans[span_of(&a, 1, 1)];
    assert_eq!((p.inclusive(), p.exclusive()), (Some(200), None));
    let text = report::render(&a);
    assert!(text.contains("inclusive 200, exclusive unavailable"), "{text}");
}

// ---- review additions: A/B compatibility policy ------------------------------------------------------------------

/// `clean_capture()` with its HEADER and CLOCKS replaced.
fn variant(hdr: Hdr, freq: u64, descs: Option<Vec<Desc>>) -> Vec<u8> {
    let recs = seqd(
        0,
        [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 210, 1)], zone(0, 1, 0, 300, 2, 0), vec![end(0, 1, 0, 360, 2)]]
            .concat(),
    );
    let mut s = scenario(vec![lane(1, recs)]);
    if let Some(ds) = descs {
        s.descs = ds;
    }
    let mut f = s.file();
    f.chunks[0] = chunk(T_HEADER, hdr.payload());
    f.chunks[1] = chunk(T_CLOCKS, clocks_payload(1, 1, 0, freq, 1000));
    f.bytes()
}

const FREQ: u64 = 79_800_000;

#[test]
fn compare_refuses_a_different_clock_frequency() {
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr::default(), FREQ + 1, None));
    let c = compare(&a, &b);
    assert!(c.refused.as_deref().unwrap().starts_with("clock frequency differs"), "{c:?}");
    assert!(c.rows.iter().all(|r| matches!(r.result, AbResult::Refused(_))));
    assert_eq!(c.freq_hz, None);
}

#[test]
fn compare_refuses_an_abi_mismatch() {
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr { abi: 2, ..Hdr::default() }, FREQ, None));
    assert_eq!(compare(&a, &b).refused.as_deref(), Some("ABI differs"));
}

#[test]
fn compare_refuses_a_descriptor_scope_mismatch() {
    let mut ds = std_descs();
    ds[0].scope = 2;
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr::default(), FREQ, Some(ds)));
    let c = compare(&a, &b);
    assert_eq!(c.refused, None);
    let za = c.rows.iter().find(|r| r.name == "zoneA").unwrap();
    assert_eq!(za.result, AbResult::Refused("descriptor scope differs (A cpu, B gpu-complete)".to_string()));
    let zb = c.rows.iter().find(|r| r.name == "zoneB").unwrap();
    assert!(matches!(zb.result, AbResult::Compared { .. }));
}

#[test]
fn compare_records_build_compiler_and_flags_differences() {
    // The intended variables of an A/B: recorded, not refused.
    let build = b"test-build-0002".to_vec();
    let mut ds = std_descs();
    for d in &mut ds {
        d.id = Some(desc_id(&build, "main.c", d.line, std::str::from_utf8(&d.name).unwrap()));
    }
    let hdr = Hdr { build_id: build, compiler: b"gcc 16.2.0".to_vec(), flags: b"-O3".to_vec(), ..Hdr::default() };
    let a = complete(&clean_capture());
    let b = complete(&variant(hdr, FREQ, Some(ds)));
    let c = compare(&a, &b);
    assert_eq!(c.kind, ComparisonKind::Compatible);
    assert_eq!(c.refused, None);
    assert_eq!(c.freq_hz, Some(FREQ));
    for what in ["build id", "compiler", "flags"] {
        assert!(c.differences.iter().any(|d| d.starts_with(what)), "{what}: {:?}", c.differences);
    }
    assert_eq!(c.differences.len(), 3, "{:?}", c.differences);
    assert!(c.rows.iter().all(|r| matches!(r.result, AbResult::Compared { .. })));
}

#[test]
fn compare_refuses_a_different_workload_on_the_same_build() {
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr { workload: b"other".to_vec(), ..Hdr::default() }, FREQ, None));
    let c = compare(&a, &b);
    assert_eq!(c.kind, ComparisonKind::Refused);
    assert!(c.refused.as_deref().unwrap().starts_with("different workload or app"), "{c:?}");
    assert!(c.rows.iter().all(|r| matches!(r.result, AbResult::Refused(_))));
    assert!(report::render_comparison(&c).starts_with("COMPARISON REFUSED: different workload or app"));
}

#[test]
fn compare_refuses_a_different_app() {
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr { app: b"other-app".to_vec(), ..Hdr::default() }, FREQ, None));
    let c = compare(&a, &b);
    assert_eq!(c.kind, ComparisonKind::Refused);
    assert!(c.refused.as_deref().unwrap().contains("app: A \"app\", B \"other-app\""), "{c:?}");
}

#[test]
fn compare_cross_workload_override_is_descriptive_never_compared() {
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr { workload: b"other".to_vec(), ..Hdr::default() }, FREQ, None));
    let c = compare_with(&a, &b, CompareOptions { allow_cross_workload: true });
    assert_eq!(c.kind, ComparisonKind::DescriptiveCrossWorkload);
    assert_eq!(c.refused, None);
    assert_eq!(c.cross_workload, vec!["workload: A \"unit\", B \"other\"".to_string()]);
    assert!(!c.rows.is_empty());
    assert!(c.rows.iter().all(|r| matches!(r.result, AbResult::Descriptive { .. })), "{:?}", c.rows);
    let text = report::render_comparison(&c);
    let first = text.lines().next().unwrap();
    assert!(first.starts_with("DESCRIPTIVE CROSS-WORKLOAD COMPARISON, NOT AN A/B RESULT"), "{text}");
}

#[test]
fn compare_same_workload_control_is_compared() {
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr::default(), FREQ, None));
    let c = compare_with(&a, &b, CompareOptions { allow_cross_workload: true });
    assert_eq!(c.kind, ComparisonKind::Compatible, "the override changes nothing for the same workload");
    assert!(c.cross_workload.is_empty());
    let zb = c.rows.iter().find(|r| r.name == "zoneB").unwrap();
    assert_eq!(zb.result, AbResult::Compared { a_mean: 60.0, b_mean: 60.0 });
    assert!(report::render_comparison(&c).starts_with("A/B COMPARISON (compatible captures)"));
}

#[test]
fn frame_clock_anomaly_invalidates_the_owner_and_refuses_frame_elapsed() {
    // Owner 1: FRAME 0 at tb 300, FRAME 1 at tb 100 (decreasing), FRAME 2 at tb 400.
    let recs = vec![frame(0, 300, 0), frame(1, 100, 1), frame(2, 400, 2)];
    let a = complete(&scenario(vec![lane(1, recs)]).bytes());
    assert!(!a.lane_valid(1));
    assert!(lane_reasons(&a, 1).iter().any(|x| x == "CLOCK_ANOMALY"));
    for k in 0..3 {
        assert_eq!(a.elapsed_for(1, k), Some(&Metric::Refused("owner invalid".to_string())), "frame {k}");
    }
    let text = report::render(&a);
    for k in 0..3 {
        let line = format!("  owner 1 frame {k}: refused (owner invalid)");
        assert!(text.lines().any(|l| l == line), "{line:?} not in\n{text}");
    }
}

#[test]
fn compare_records_a_source_line_difference() {
    let mut ds = std_descs();
    ds[1].line = 99;
    let a = complete(&clean_capture());
    let b = complete(&variant(Hdr::default(), FREQ, Some(ds)));
    let c = compare(&a, &b);
    let zb = c.rows.iter().find(|r| r.name == "zoneB").unwrap();
    assert!(matches!(zb.result, AbResult::Compared { .. }));
    assert_eq!(zb.differences, vec!["source location: A main.c:11, B main.c:99".to_string()]);
    assert!(c.rows.iter().find(|r| r.name == "zoneA").unwrap().differences.is_empty());
}

#[test]
fn compare_exposes_the_valid_lane_scope() {
    let mut bad = lane(2, Vec::new());
    bad.regerr = 1;
    let a = complete(&scenario(vec![lane(1, one_zone()), bad]).bytes());
    let b = complete(&clean_capture());
    let c = compare(&a, &b);
    assert_eq!((c.a_scope.clone(), c.b_scope.clone()), (vec![1], vec![1]));
}

#[test]
fn compare_distinguishes_a_key_only_in_invalid_lanes_from_an_absent_key() {
    // A: lane 1 (valid) has zoneA and zoneC; lane 2 (invalid: regerr) has zoneB.  B: lane 1 has zoneA only.
    let l1 = seqd(
        0,
        [zone(0, 0, 0, 100, 1, 0), vec![end(0, 0, 0, 200, 1)], zone(0, 2, 0, 300, 2, 0), vec![end(0, 2, 0, 350, 2)]]
            .concat(),
    );
    let mut l2 = lane(2, seqd(0, [zone(0, 1, 0, 120, 1, 0), vec![end(0, 1, 0, 180, 1)]].concat()));
    l2.regerr = 1;
    let a = complete(&scenario(vec![lane(1, l1), l2]).bytes());
    let b = complete(&scenario(vec![lane(1, one_zone())]).bytes());
    let c = compare(&a, &b);
    let row = |n: &str| c.rows.iter().find(|r| r.name == n).unwrap().result.clone();
    assert!(matches!(row("zoneA"), AbResult::Compared { .. }));
    assert_eq!(row("zoneB"), AbResult::InvalidLanesOnly { a: true, b: false });
    assert_eq!(row("zoneC"), AbResult::OnlyInA);
}

// ---- review additions: one live incarnation per slot (section 3) ----------------------------------------------------

#[test]
fn slot_reuse_after_its_incarnation_retired_is_valid() {
    let a = complete(&slot_reuse_file(true));
    assert!(a.lane_valid(1) && a.lane_valid(2));
    let slots: Vec<u32> = a.capture.lanes.values().map(|l| l.info.as_ref().unwrap().slot).collect();
    assert_eq!(slots, vec![1, 1]);
}

// ---- review additions: read is structural, load is the verdict --------------------------------------------------------

#[test]
fn read_alone_accepts_a_rule_4_case_that_load_refuses() {
    let bytes = token_scenario(5).bytes();
    assert!(reader::read(&bytes).is_ok(), "the structural pass accepts it");
    match load(&bytes) {
        Outcome::Malformed(m) => assert_eq!(m.rule, Rule::TokenImpossible),
        Outcome::Analysed(a) => panic!("load must refuse it, got {:?}", a.verdict),
    }
}

// ---- producer effects a file can show (consistency only; producer behaviour itself is stage 3) ------------------------

#[test]
fn acked_entry_with_seq_limit_and_an_unadmitted_stop_batch_is_consistent() {
    // produced_count had reached 2^63 with two spans open: the STOP batch of k = 2 ENDs was not admitted, so
    // refused_count = k, open_spans = k, SEQ_LIMIT is set and no TRUNCATED_AT_STOP END was written.
    let recs = seqd(0, [vec![gap(0, P63 - 5)], zone(0, 0, 0, 100, 1, 0), zone(0, 1, 1, 150, 2, 1)].concat());
    let mut l = lane(1, recs);
    l.limit = true;
    l.refused = 2;
    assert_eq!(l.fin().produced, P63);
    let s = scenario(vec![l]);
    let mut t = s.trailer_struct();
    t.fins[0].open = 2;
    let mut f = s.file();
    let i = f.find(T_TRAILER);
    f.chunks[i] = chunk(T_TRAILER, t.payload());
    let (a, why) = incomplete(&f.bytes());
    assert_eq!(why, vec![Incomplete::SeqLimit]);
    let e = a.capture.lanes[&1].final_entry.as_ref().unwrap();
    assert_eq!((e.state, e.open_spans, e.refused_count), (2, 2, 2));
    let spans: Vec<_> = a.capture.spans.iter().filter(|s| s.incarnation == 1).collect();
    assert_eq!(spans.len(), 2);
    assert!(spans.iter().all(|s| s.is_open() && !s.truncated_at_stop), "{spans:?}");
}

#[test]
fn retired_entry_with_open_spans_2_and_two_open_spans_in_the_records() {
    // thread_end ran with two spans open: it wrote no END, so both stay OPEN (censored) in the file.
    let recs = seqd(0, [zone(0, 0, 0, 100, 1, 0), zone(0, 1, 1, 150, 2, 1)].concat());
    let mut l = lane(1, recs);
    l.state = 1;
    let mut fin = l.fin();
    fin.open = 2;
    let mut f = scenario(vec![l]).file();
    let i = f.find(T_RETIRED);
    f.chunks[i] = chunk(T_RETIRED, retired_payload(&[fin]));
    let a = complete(&f.bytes());
    assert!(a.lane_valid(1));
    let e = a.capture.lanes[&1].final_entry.as_ref().unwrap();
    assert_eq!((e.state, e.open_spans), (1, 2));
    let spans: Vec<_> = a.capture.spans.iter().filter(|s| s.incarnation == 1).collect();
    assert_eq!(spans.len(), 2);
    assert!(spans.iter().all(|s| s.is_open() && s.is_censored()));
    assert_eq!(a.zone("zoneA").unwrap().label.as_deref(), Some("completed instances only: 1 censored of 1"));
    let text = report::render(&a);
    assert_eq!(text.lines().filter(|l| l.contains("duration unknown (censored)") && l.contains("OPEN")).count(), 2);
}
