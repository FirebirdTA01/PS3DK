"""Generate tests/SPEC-COVERAGE.md, a TRACEABILITY MAP from spec bullets and Rule variants to tests.
usage (from tools/ps3dk-prof): python3 -I -B tests/gen_spec_coverage.py .
It refuses a Rule variant with no asserting test, and a mapped test name that does not exist.  It cannot prove that
every normative rule is implemented: the bullet list below is written by hand, and the rule list is the Rule enum."""
import pathlib
import re
import sys

C = pathlib.Path(sys.argv[1])
model = (C / 'src/model.rs').read_text()
body = model.split('pub enum Rule {', 1)[1].split('\n}', 1)[0]
rules = re.findall(r'^\s+([A-Z][A-Za-z0-9]+),', body, re.M)


def tests(path):
    t = path.read_text()
    out = {}
    for m in re.finditer(r'#\[test\]\s*fn (\w+)\(\)\s*\{(.*?)\n\}', t, re.S):
        out[m.group(1)] = re.findall(r'Rule::(\w+)', m.group(2))
    return out


mt = tests(C / 'tests/malformed.rs')
st = tests(C / 'tests/stage2.rs')
ct = tests(C / 'tests/cli.rs')
cov = {r: [] for r in rules}
for name, rs in list(mt.items()) + list(st.items()) + list(ct.items()):
    for r in rs:
        if name not in cov[r]:
            cov[r].append(name)
missing = [r for r in rules if not cov[r]]
if missing:
    sys.exit('rules without a test: %s' % missing)

B = [
    ("Base", "every MALFORMED rule, one fixture each", None),
    ("Base", "the torn tail, giving INCOMPLETE", ["torn_tail_gives_incomplete"]),
    ("Base", "BEGIN_PARENT split across chunks", ["begin_parent_split_across_chunks"]),
    ("Base", "an adjacent GAP pair, and a GAP at the end of a chunk", ["adjacent_gap_pair", "gap_at_end_of_a_chunk"]),
    ("Base", "produced_count = 2^63 accepted and 2^63 + 1 refused",
     ["produced_count_2_pow_63_accepted", "produced_count_2_pow_63_plus_1_refused"]),
    ("Base", "a 2-slot BEGIN at 2^63 - 1 refused, with produced_count at 2^63 - 1",
     ["two_slot_begin_at_2_pow_63_minus_1_refused"]),
    ("Base", "a REGERR dropped while regerr_count > 0, which must give INVALID and must not look clean",
     ["regerr_dropped_while_regerr_count_positive_is_invalid_not_clean"]),
    ("Base", "consistency arithmetic, including terminal tail = 1 with written_count = 0",
     ["consistency_arithmetic_terminal_tail_1_with_written_0", "consistency_arithmetic_empty_lane_is_valid",
      "malformed.rs: written_count_mismatch", "produced_count_range", "produced_count_arithmetic",
      "dropped_count_arithmetic"]),
    ("Base", "RETIRED-then-RECORDS", ["retired_then_records", "malformed.rs: records_after_retired"]),
    ("Base", "a decreasing tb, which gives CLOCK_ANOMALY", ["decreasing_tb_gives_clock_anomaly"]),
    ("Base", "a frame-id hole with and without loss", ["frame_id_hole_without_loss", "frame_id_hole_with_loss"]),
    ("Ancestry", "a parent BEGIN dropped in a GAP, followed by the child BEGIN, the child END and the parent END",
     ["parent_begin_dropped_then_child_begin_child_end_parent_end"]),
    ("Ancestry", "the partial-stack checks while the base is UNKNOWN: a wrong child depth or parent above a seen span",
     ["partial_stack_with_unknown_base_wrong_child_depth", "partial_stack_with_unknown_base_wrong_parent"]),
    ("Ancestry", "a depth-0 resync, after which a wrong parent IS MALFORMED",
     ["depth_0_resync_then_wrong_parent_is_malformed", "depth_0_resync_makes_base_known"]),
    ("Ancestry", "a depth-0 BEGIN while the partial stack is non-empty, MALFORMED",
     ["depth_0_begin_while_partial_stack_non_empty"]),
    ("Ancestry", "depth/parent zero mismatch, MALFORMED in either base state",
     ["depth_parent_zero_mismatch_base_known", "depth_parent_zero_mismatch_base_unknown"]),
    ("Ancestry", "an END on an empty stack: MALFORMED with base KNOWN, AFFECTED with base UNKNOWN",
     ["end_on_empty_stack_base_known_is_malformed", "end_on_empty_stack_base_unknown_is_affected"]),
    ("Final entries", "a state-3 entry with exactly the UNKNOWN fields and limit_flags 0x02, accepted as INCOMPLETE",
     ["state_3_entry_with_exact_unknown_fields_is_incomplete"]),
    ("Final entries", "a state-3 entry with a concrete produced_count, MALFORMED",
     ["state_3_entry_with_concrete_produced_count"]),
    ("Final entries", "a state-1 or state-2 entry containing UNKNOWN, MALFORMED",
     ["state_1_entry_containing_unknown", "state_2_entry_containing_unknown"]),
    ("Final entries",
     "a SEQ_LIMIT only in an earlier RETIRED entry: bit1 required (INCOMPLETE); bit1 missing; bit1 with no limited entry",
     ["seq_limit_only_in_earlier_retired_entry_bit1_required", "seq_limit_only_in_earlier_retired_entry_bit1_missing",
      "seq_limit_bit1_with_no_limited_entry"]),
    ("Censoring", "a zone with one truncated instance: labelled, A/B refused CENSORED",
     ["zone_with_one_truncated_instance_is_labelled_and_ab_refused_censored"]),
    ("Censoring", "a clean zone in the same capture compares normally",
     ["clean_zone_in_the_same_capture_compares_normally"]),
    ("Censoring", "the unclosed last frame is censored", ["unclosed_last_frame_is_censored"]),
    ("Frames and tokens", "a future-arriving valid frame gives RESOLVED and no error",
     ["future_arriving_valid_frame_resolves"]),
    ("Frames and tokens", "a nonexistent frame in a complete capture with a lossless, quiesced owner: MALFORMED",
     ["nonexistent_frame_complete_lossless_quiesced_owner_is_malformed"]),
    ("Frames and tokens",
     "the same reference with owner loss: UNRESOLVED-LOSS; owner frame elapsed and every W refused; worker statistics kept",
     ["nonexistent_frame_with_owner_loss_is_unresolved_loss"]),
    ("Frames and tokens", "a clean worker with an invalid owner", ["clean_worker_with_invalid_owner"]),
    ("Frames and tokens", "un-tokened owner span before its first FRAME (pre-frame) and after a GAP (loss)",
     ["owner_untokened_span_before_first_frame_is_pre_frame",
      "owner_untokened_span_after_gap_before_next_frame_is_loss"]),
    ("Token resolution", "present FRAME in an INCOMPLETE (no-trailer) capture: RESOLVED (rule 1)",
     ["rule1_present_frame_in_no_trailer_capture_resolves"]),
    ("Token resolution", "missing: no TRAILER; torn tail; O has no final entry (rule 2)",
     ["rule2_missing_no_trailer", "rule2_missing_torn_tail", "rule2_missing_owner_has_no_final_entry"]),
    ("Token resolution", "missing: O has a GAP; terminal loss only; O is state 3 (rule 3)",
     ["rule3_missing_owner_has_gap", "rule3_missing_owner_has_terminal_loss_only", "rule3_missing_owner_is_state_3"]),
    ("Token resolution", "missing, INCOMPLETE only through SEQ_LIMIT, on another lane and on O itself: MALFORMED",
     ["rule4_missing_incomplete_only_through_seq_limit_on_another_lane",
      "rule4_missing_incomplete_only_through_seq_limit_on_owner"]),
    ("Token resolution", "missing, only through a recovered sink error (bit2): MALFORMED",
     ["rule4_missing_incomplete_only_through_recovered_sink_error"]),
    ("Token resolution", "missing, only through another lane NOT_QUIESCED (bit0): MALFORMED",
     ["rule4_missing_incomplete_only_through_other_lane_not_quiesced"]),
    ("Token resolution", "missing, complete lossless capture: MALFORMED (rule 4)",
     ["rule4_missing_complete_lossless_capture", "malformed.rs: token_impossible"]),
    ("Attributed work", "nested parent and child on one lane: exclusive sum = parent inclusive, no double count",
     ["nested_parent_and_child_exclusive_sum_to_parent_inclusive"]),
    ("Attributed work", "a child in a later frame than its parent", ["child_in_a_later_frame_than_its_parent"]),
    ("Attributed work", "two overlapping workers tokened to the same frame: two W values, no cross-lane sum",
     ["two_overlapping_workers_tokened_to_the_same_frame"]),
    ("Attributed work", "a clean owner and a clean worker plus a third, invalid lane",
     ["clean_owner_clean_worker_and_invalid_third_lane"]),
    ("Spans N", "an owner span crossing two FRAMEs gives spans 3 frames", ["owner_span_crossing_two_frames_spans_3"]),
    ("Spans N", "a tokened worker span gives spans N: unknown", ["tokened_worker_span_n_unknown"]),
    ("Spans N", "an owner span with a GAP inside it gives N unknown", ["owner_span_with_gap_inside_n_unknown"]),
]

# Review additions (codex, 2026-10-21 22:41, 22:42 and 23:00): rules and behaviours the stage-2 reviews found missing.
R = [
    ("1 exact counts", "written, dropped, refused and regerr above their 2^63 representation ceiling are MALFORMED",
     ["malformed.rs: written_count_over_2_pow_63", "malformed.rs: dropped_count_over_2_pow_63",
      "malformed.rs: refused_count_over_2_pow_63", "malformed.rs: regerr_count_over_2_pow_63",
      "malformed.rs: state3_written_count_over_2_pow_63"]),
    ("1 exact counts", "2^63 is accepted (written_count = 2^63 cannot be built: it must equal the records written)",
     ["dropped_count_2_pow_63_accepted", "refused_count_2_pow_63_accepted", "regerr_count_2_pow_63_accepted"]),
    ("1 open_spans", "an active-stack snapshot: 256 accepted, 257 MALFORMED, UNKNOWN accepted in state 3",
     ["open_spans_256_accepted", "malformed.rs: open_spans_257_is_malformed", "state3_open_spans_unknown_accepted"]),
    ("1 REGERR", "regerr_count must cover the REGERR records written; a nonzero count invalidates the lane",
     ["malformed.rs: regerr_record_with_regerr_count_0", "regerr_record_with_nonzero_regerr_count_makes_lane_invalid"]),
    ("2 FRAME ids", "no saturation: repeated after a GAP, u64::MAX, above its own seq; the maximum legal id",
     ["malformed.rs: frame_id_repeated_after_gap", "malformed.rs: frame_id_u64_max_after_gap",
      "malformed.rs: frame_id_one_above_its_seq", "frame_id_equal_to_its_seq_at_the_maximum_is_accepted"]),
    ("3 no saturated value", "exact totals; unavailable (never 0) durations in analysis and report",
     ["nested_same_zone_spans_of_2_pow_63_have_an_exact_total_and_mean",
      "clock_anomaly_span_duration_is_unavailable_not_zero",
      "parent_of_an_anomalous_child_has_an_unavailable_exclusive_duration"]),
    ("3 frame clock", "decreasing FRAME tb invalidates the owner; frame elapsed refused, no number in the report",
     ["frame_clock_anomaly_invalidates_the_owner_and_refuses_frame_elapsed"]),
    ("4 A/B policy", "clock, ABI and scope refusals; recorded differences; valid-lane scope; invalid-only versus absent",
     ["compare_refuses_a_different_clock_frequency", "compare_refuses_an_abi_mismatch",
      "compare_refuses_a_descriptor_scope_mismatch", "compare_records_build_compiler_and_flags_differences",
      "compare_records_a_source_line_difference", "compare_exposes_the_valid_lane_scope",
      "compare_distinguishes_a_key_only_in_invalid_lanes_from_an_absent_key"]),
    ("4 A/B workload", "a different workload or app is refused; the explicit override is descriptive, never Compared",
     ["compare_refuses_a_different_workload_on_the_same_build", "compare_refuses_a_different_app",
      "compare_cross_workload_override_is_descriptive_never_compared", "compare_same_workload_control_is_compared"]),
    ("5 slots", "one live incarnation per slot; reuse after RETIRED is valid",
     ["malformed.rs: slot_already_live", "malformed.rs: slot_reused_before_its_incarnation_retired",
      "slot_reuse_after_its_incarnation_retired_is_valid"]),
    ("6 short header", "every constrained byte of a cut header is checked",
     ["malformed.rs: short_header_valid_prefix_at_every_length_is_torn",
      "malformed.rs: short_header_bad_byte_at_every_cut_point",
      "malformed.rs: fifteen_byte_header_with_nonzero_flags_byte_is_not_torn"]),
    ("16 read versus load", "read is structural; check, report and A/B use the whole-file verdict",
     ["read_alone_accepts_a_rule_4_case_that_load_refuses",
      "cli.rs: check_reports_malformed_for_a_rule_4_case_the_structural_pass_accepts",
      "cli.rs: report_also_refuses_the_rule_4_case", "cli.rs: check_exit_status_for_complete_and_incomplete"]),
]

for g, b, ts in B + R:
    for t in ts or []:
        name = t.split(': ')[-1]
        if name not in st and name not in mt and name not in ct:
            sys.exit('bullet %r names unknown test %s' % (b, name))

md = [
    "# ps3dk-prof spec traceability map (stage 2, reader)",
    "",
    "A TRACEABILITY MAP, not a proof of coverage: it links each bullet of docs/design/ps3dk-prof-format.md section 10,",
    "\"Stage 2, reader\", each review addition, and each `Rule` variant to the tests that exercise it.  The bullet",
    "lists are written by hand and the rule list is the `Rule` enum, so a normative rule with no `Rule` variant would",
    "not appear here.  Generated by tests/gen_spec_coverage.py, which refuses a `Rule` variant with no asserting test",
    "and a mapped test name that does not exist.  Stage 3 and later bullets are not part of stage 2.",
    "",
    "## Section 10 bullets (%d) -> tests/stage2.rs unless marked" % len(B),
    "",
    "| group | bullet | tests |",
    "|---|---|---|",
]
for g, b, ts in B:
    cell = 'all of tests/malformed.rs (table below)' if ts is None else ', '.join('`%s`' % t for t in ts)
    md.append('| %s | %s | %s |' % (g, b, cell))
md += [
    "",
    "## Review additions (%d) -> tests/stage2.rs unless marked" % len(R),
    "",
    "| item | behaviour | tests |",
    "|---|---|---|",
]
for g, b, ts in R:
    md.append('| %s | %s | %s |' % (g, b, ', '.join('`%s`' % t for t in ts)))
md += [
    "",
    "## Rule variants (%d) -> tests (%d in malformed.rs, %d in stage2.rs, %d in cli.rs)"
    % (len(rules), len(mt), len(st), len(ct)),
    "",
    "| rule | tests |",
    "|---|---|",
]
for r in rules:
    md.append('| `%s` | %s |' % (r, ', '.join('`%s`' % n for n in cov[r])))
(C / 'tests/SPEC-COVERAGE.md').write_bytes(('\n'.join(md) + '\n').encode())
print('%d bullets, %d review additions, %d rules, %d + %d + %d tests' % (len(B), len(R), len(rules), len(mt), len(st), len(ct)))
