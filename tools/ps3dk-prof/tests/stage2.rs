//! Section 10, "Stage 2, reader": one test per listed fixture, named after its bullet.  The "every MALFORMED rule"
//! bullet is tests/malformed.rs.  tests/SPEC-COVERAGE.md maps each bullet to its test.

mod common;

use common::*;
use ps3dk_prof::{compare, report, AbResult, Attribution, Incomplete, Metric, Rule, SpansN, TokenOutcome};

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
    let rows = compare(&a, &b);
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
    let rows = compare(&a, &b);
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
    assert_eq!(a.work_for(1, 0, 3), Some(&Metric::Refused("lane invalid".to_string())));
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
