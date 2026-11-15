#include <stdio.h>
#include <string.h>
#include "../../samples/gcm/hello-ppu-gcm-report-ring/source/probe_logic.h"
#define CHECK(c, name) do { if (!(c)) { puts("ASSERTION: " name); return 1; } } while (0)
int main(void) {
    CHECK(rrp_wait(120, 120, 1) == RRP_EXPIRED, "deadline_before_completed_label");
    CHECK(rrp_wait(119, 120, 1) == RRP_DONE, "wait_done");
    CHECK(rrp_wait(119, 120, 0) == RRP_WAIT, "wait_pending");
    CHECK(rrp_verdict(8, 0, 0, 8) == RRP_SUPPORTED, "sensitive_clean_series");
    CHECK(rrp_verdict(8, 0, 0, 0) == RRP_INCONCLUSIVE, "no_control_no_support");
    CHECK(rrp_verdict(7, 0, 0, 8) == RRP_INCONCLUSIVE, "missing_trial_no_support");
    CHECK(rrp_verdict(8, 1, 0, 0) == RRP_REFUTED, "candidate_counterexample");
    CHECK(rrp_verdict(8, 1, 1, 8) == RRP_INVALID, "bad_reference_precedence");
    CHECK(rrp_local_span(0xc0000000u, 0x1000000u, 0xc0001000u, 4032, 0x1000), "local_span");
    CHECK(!rrp_local_span(0xc0000000u, 0x1000000u, 0x40301000u, 4032, 0x1000), "nonlocal_report_mapping");
    CHECK(!rrp_local_span(0xc0000000u, 0x1000000u, 0xc0ffffffu, 4032, 0xffffff), "source_overrun");
    CHECK(!rrp_local_span(0xc0000000u, 0x1000000u, 0xc0001000u, 4032, 0x2000), "translated_offset_mismatch");
    CHECK(rrp_local_span(UINT64_MAX-31, 32, UINT64_MAX-15, 16, 16), "nonwrapping_bounds");
    CHECK(!rrp_local_span(UINT64_MAX-31, 32, UINT64_MAX-15, 17, 16), "overflow_bounds");
    const unsigned char a1[] = {RRP_WRITE, RRP_REPORT_FENCE, RRP_COPY_OLD, RRP_PUBLISH, RRP_END};
    const unsigned char a2[] = {RRP_COPY_OLD, RRP_COPY_FENCE, RRP_WRITE, RRP_REPORT_FENCE,
                                RRP_COPY_NEW, RRP_PUBLISH, RRP_END};
    const unsigned char b2[] = {RRP_EARLY, RRP_GATE, RRP_COPY_OLD, RRP_END};
    CHECK(!memcmp(rrp_plan(RRP_A1, 0), a1, sizeof a1), "a1_report_before_copy");
    CHECK(!memcmp(rrp_plan(RRP_A2, 0), a2, sizeof a2), "a2_copy_before_reuse");
    CHECK(!memcmp(rrp_plan(RRP_B2, 1), b2, sizeof b2), "b2_early_gate_control");
    CHECK(rrp_plan(RRP_A1, 1)[0] == RRP_COPY_OLD, "a1_early_control");
    CHECK(rrp_plan(RRP_A2, 1)[0] == RRP_WRITE, "a2_overwrite_control");
    CHECK(rrp_plan(RRP_B1, 1)[0] == RRP_COPY_BAD, "b1_wrong_source_control");
    CHECK(RRP_REPORT_FIRST + 4 + 2*RRP_MAX_PAIRS*2 + 4 - 1 == 1023, "report_partition");
    puts("report-ring probe checks PASS");
    return 0;
}
