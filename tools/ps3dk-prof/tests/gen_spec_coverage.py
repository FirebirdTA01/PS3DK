"""Generate tests/SPEC-COVERAGE.md from the test sources.  usage (from tools/ps3dk-prof): python3 -I -B tests/gen_spec_coverage.py ."""
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
cov = {r: [] for r in rules}
for name, rs in list(mt.items()) + list(st.items()):
    for r in rs:
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
    ("Ancestry (r2)", "a parent BEGIN dropped in a GAP, followed by the child BEGIN, the child END and the parent END",
     ["parent_begin_dropped_then_child_begin_child_end_parent_end"]),
    ("Ancestry (r2)", "the partial-stack checks while the base is UNKNOWN: a wrong child depth or parent above a seen span",
     ["partial_stack_with_unknown_base_wrong_child_depth", "partial_stack_with_unknown_base_wrong_parent"]),
    ("Ancestry (r2)", "a depth-0 resync, after which a wrong parent IS MALFORMED",
     ["depth_0_resync_then_wrong_parent_is_malformed", "depth_0_resync_makes_base_known"]),
    ("Ancestry (r2)", "a depth-0 BEGIN while the partial stack is non-empty, MALFORMED",
     ["depth_0_begin_while_partial_stack_non_empty"]),
    ("Ancestry (r2)", "depth/parent zero mismatch, MALFORMED in either base state",
     ["depth_parent_zero_mismatch_base_known", "depth_parent_zero_mismatch_base_unknown"]),
    ("Ancestry (r2)", "an END on an empty stack: MALFORMED with base KNOWN, AFFECTED with base UNKNOWN",
     ["end_on_empty_stack_base_known_is_malformed", "end_on_empty_stack_base_unknown_is_affected"]),
    ("Final entries (r2)", "a state-3 entry with exactly the UNKNOWN fields and limit_flags 0x02, accepted as INCOMPLETE",
     ["state_3_entry_with_exact_unknown_fields_is_incomplete"]),
    ("Final entries (r2)", "a state-3 entry with a concrete produced_count, MALFORMED",
     ["state_3_entry_with_concrete_produced_count"]),
    ("Final entries (r2)", "a state-1 or state-2 entry containing UNKNOWN, MALFORMED",
     ["state_1_entry_containing_unknown", "state_2_entry_containing_unknown"]),
    ("Final entries (r2)",
     "a SEQ_LIMIT only in an earlier RETIRED entry: bit1 required (INCOMPLETE); bit1 missing; bit1 with no limited entry",
     ["seq_limit_only_in_earlier_retired_entry_bit1_required", "seq_limit_only_in_earlier_retired_entry_bit1_missing",
      "seq_limit_bit1_with_no_limited_entry"]),
    ("Censoring (r2)", "a zone with one truncated instance: labelled, A/B refused CENSORED",
     ["zone_with_one_truncated_instance_is_labelled_and_ab_refused_censored"]),
    ("Censoring (r2)", "a clean zone in the same capture compares normally",
     ["clean_zone_in_the_same_capture_compares_normally"]),
    ("Censoring (r2)", "the unclosed last frame is censored", ["unclosed_last_frame_is_censored"]),
    ("Frames and tokens (r3)", "a future-arriving valid frame gives RESOLVED and no error",
     ["future_arriving_valid_frame_resolves"]),
    ("Frames and tokens (r3)", "a nonexistent frame in a complete capture with a lossless, quiesced owner: MALFORMED",
     ["nonexistent_frame_complete_lossless_quiesced_owner_is_malformed"]),
    ("Frames and tokens (r3)",
     "the same reference with owner loss: UNRESOLVED-LOSS; owner frame elapsed and every W refused; worker statistics kept",
     ["nonexistent_frame_with_owner_loss_is_unresolved_loss"]),
    ("Frames and tokens (r3)", "a clean worker with an invalid owner", ["clean_worker_with_invalid_owner"]),
    ("Frames and tokens (r3)", "un-tokened owner span before its first FRAME (pre-frame) and after a GAP (loss)",
     ["owner_untokened_span_before_first_frame_is_pre_frame",
      "owner_untokened_span_after_gap_before_next_frame_is_loss"]),
    ("Token resolution (r4)", "present FRAME in an INCOMPLETE (no-trailer) capture: RESOLVED (rule 1)",
     ["rule1_present_frame_in_no_trailer_capture_resolves"]),
    ("Token resolution (r4)", "missing: no TRAILER; torn tail; O has no final entry (rule 2)",
     ["rule2_missing_no_trailer", "rule2_missing_torn_tail", "rule2_missing_owner_has_no_final_entry"]),
    ("Token resolution (r4)", "missing: O has a GAP; terminal loss only; O is state 3 (rule 3)",
     ["rule3_missing_owner_has_gap", "rule3_missing_owner_has_terminal_loss_only", "rule3_missing_owner_is_state_3"]),
    ("Token resolution (r4)", "missing, INCOMPLETE only through SEQ_LIMIT, on another lane and on O itself: MALFORMED",
     ["rule4_missing_incomplete_only_through_seq_limit_on_another_lane",
      "rule4_missing_incomplete_only_through_seq_limit_on_owner"]),
    ("Token resolution (r4)", "missing, only through a recovered sink error (bit2): MALFORMED",
     ["rule4_missing_incomplete_only_through_recovered_sink_error"]),
    ("Token resolution (r4)", "missing, only through another lane NOT_QUIESCED (bit0): MALFORMED",
     ["rule4_missing_incomplete_only_through_other_lane_not_quiesced"]),
    ("Token resolution (r4)", "missing, complete lossless capture: MALFORMED (rule 4)",
     ["rule4_missing_complete_lossless_capture", "malformed.rs: token_impossible"]),
    ("Attributed work (r4)", "nested parent and child on one lane: exclusive sum = parent inclusive, no double count",
     ["nested_parent_and_child_exclusive_sum_to_parent_inclusive"]),
    ("Attributed work (r4)", "a child in a later frame than its parent", ["child_in_a_later_frame_than_its_parent"]),
    ("Attributed work (r4)", "two overlapping workers tokened to the same frame: two W values, no cross-lane sum",
     ["two_overlapping_workers_tokened_to_the_same_frame"]),
    ("Attributed work (r4)", "a clean owner and a clean worker plus a third, invalid lane",
     ["clean_owner_clean_worker_and_invalid_third_lane"]),
    ("Spans N (r4)", "an owner span crossing two FRAMEs gives spans 3 frames", ["owner_span_crossing_two_frames_spans_3"]),
    ("Spans N (r4)", "a tokened worker span gives spans N: unknown", ["tokened_worker_span_n_unknown"]),
    ("Spans N (r4)", "an owner span with a GAP inside it gives N unknown", ["owner_span_with_gap_inside_n_unknown"]),
]

for g, b, ts in B:
    for t in ts or []:
        name = t.split(': ')[-1]
        if name not in st and name not in mt:
            sys.exit('bullet %r names unknown test %s' % (b, name))

md = [
    "# ps3dk-prof spec coverage (stage 2, reader)",
    "",
    "Maps every bullet of docs/design/ps3dk-prof-format.md section 10, \"Stage 2, reader\", and every MALFORMED rule of",
    "sections 1-6 and 9 to its tests.  Stage 3 and later bullets are not part of stage 2.  Generated by tests/gen_spec_coverage.py from",
    "the test sources (each MALFORMED test asserts its `Rule::` value), and the generator refuses a rule with no test.",
    "",
    "## Section 10 bullets (%d) -> tests/stage2.rs unless marked malformed.rs" % len(B),
    "",
    "| group | bullet | tests |",
    "|---|---|---|",
]
for g, b, ts in B:
    cell = 'all of tests/malformed.rs (table below)' if ts is None else ', '.join('`%s`' % t for t in ts)
    md.append('| %s | %s | %s |' % (g, b, cell))
md += [
    "",
    "## MALFORMED rules (%d) -> tests (%d in malformed.rs, %d in stage2.rs)" % (len(rules), len(mt), len(st)),
    "",
    "| rule | tests |",
    "|---|---|",
]
for r in rules:
    md.append('| `%s` | %s |' % (r, ', '.join('`%s`' % n for n in cov[r])))
(C / 'tests/SPEC-COVERAGE.md').write_bytes(('\n'.join(md) + '\n').encode())
print('%d bullets, %d rules, %d + %d tests' % (len(B), len(rules), len(mt), len(st)))
