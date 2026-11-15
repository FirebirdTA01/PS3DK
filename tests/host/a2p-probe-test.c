#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "a2p_classifier.h"

/* Negative test 1: poison colour under vertex poison must be classified as UNRELATED */
static void test_neg_poison_under_vertex(void)
{
    uint32_t good = 0xff112233u;
    int c = a2p_classify_sample(A2P_COLOR_POISON, good, A2P_POISON_VERTEX, 0);
    assert(c == A2P_SAMPLE_UNRELATED);
    printf("PASS: negative: poison colour under vertex is unrelated\n");
}

/* Negative test 2: poison colour under index poison must be classified as UNRELATED */
static void test_neg_poison_under_index(void)
{
    uint32_t good = 0xff112233u;
    int c = a2p_classify_sample(A2P_COLOR_POISON, good, A2P_POISON_INDEX, 0);
    assert(c == A2P_SAMPLE_UNRELATED);
    printf("PASS: negative: poison colour under index is unrelated\n");
}

/* Negative test 3: clear colour under texture poison must be classified as UNRELATED */
static void test_neg_clear_under_texture(void)
{
    uint32_t good = 0xff112233u;
    int c = a2p_classify_sample(A2P_COLOR_CLEAR, good, A2P_POISON_TEXTURE, 0);
    assert(c == A2P_SAMPLE_UNRELATED);
    printf("PASS: negative: clear colour under texture is unrelated\n");
}

/* Negative test 4: sentinel must be classified as UNRELATED across all modes */
static void test_neg_sentinel(void)
{
    uint32_t good = 0xff112233u;
    assert(a2p_classify_sample(A2P_SENTINEL, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(A2P_SENTINEL, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(A2P_SENTINEL, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(A2P_SENTINEL, good, A2P_POISON_NONE, 1) == A2P_SAMPLE_UNRELATED);
    printf("PASS: negative: sentinel is unrelated across all modes\n");
}

/* Negative test 5: unknown colour must be classified as UNRELATED */
static void test_neg_unknown_colour(void)
{
    uint32_t good = 0xff112233u;
    uint32_t unknown = 0xff445566u;
    assert(a2p_classify_sample(unknown, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(unknown, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(unknown, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(unknown, good, A2P_POISON_NONE, 1) == A2P_SAMPLE_UNRELATED);
    printf("PASS: negative: unknown colour is unrelated across all modes\n");
}

/* Positive controls: expected colours match their specific poison kinds */
static void test_pos_expected_controls(void)
{
    uint32_t good = 0xff112233u;
    assert(a2p_classify_sample(A2P_COLOR_CLEAR, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_EXPECTED);
    assert(a2p_classify_sample(A2P_COLOR_CLEAR, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_EXPECTED);
    assert(a2p_classify_sample(A2P_COLOR_POISON, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_EXPECTED);
    assert(a2p_classify_sample(good, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_GOOD);
    assert(a2p_classify_sample(good, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_GOOD);
    assert(a2p_classify_sample(good, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_GOOD);
    assert(a2p_classify_sample(good, good, A2P_POISON_NONE, 1) == A2P_SAMPLE_GOOD);
    printf("PASS: positive controls: expected colours match specific poison kinds\n");
}

typedef struct {
    unsigned fail_at_trial;
    unsigned calls;
    unsigned submitted;
    unsigned reset;
    unsigned rewritten;
    int outcome_to_report;
    int all_clear_to_report;
    uint64_t fake_now_us;
    uint64_t deadline_us;
    uint64_t step_per_trial_us;
    int trigger_checkpoint; /* 0: pre-trial, 1: done-label-wait, 2: pre-readback, 3: readback-wait */
    unsigned trigger_at_trial;
    const char *deadline_at;
} fake_trial_state;

static int fake_trial_fn(void *user_ctx, int mode, int poison_kind, unsigned trial,
                         int *out_trial_outcome, int *out_all_clear)
{
    (void)mode;
    (void)poison_kind;
    fake_trial_state *s = (fake_trial_state *)user_ctx;
    s->calls++;
    if (trial == s->fail_at_trial) {
        /* Simulate timeout / failure */
        return -1;
    }
    s->submitted++;
    s->reset++;
    s->rewritten++;
    *out_trial_outcome = s->outcome_to_report;
    if (out_all_clear)
        *out_all_clear = s->all_clear_to_report;
    return 0;
}

/* Negative test 6: reference trial with bad samples counts as exactly ONE failed trial in series */
static void test_reference_counted_once(void)
{
    fake_trial_state s;
    memset(&s, 0, sizeof s);
    s.fail_at_trial = 999; /* no abort */
    s.outcome_to_report = A2P_TRIAL_UNRELATED; /* trial 0 reports unrelated */

    a2p_series_counts counts;
    unsigned failed_trial = 0;

    /* First run 1 trial with unrelated */
    int rc = a2p_run_series_loop(&s, 0, A2P_POISON_NONE, 1, fake_trial_fn, &counts, &failed_trial);
    assert(rc == 0);
    assert(counts.unrelated == 1);
    assert(counts.clean == 0);
    assert(counts.expected == 0);

    /* Now run 31 clean trials */
    s.outcome_to_report = A2P_TRIAL_CLEAN;
    rc = a2p_run_series_loop(&s, 0, A2P_POISON_NONE, 31, fake_trial_fn, &counts, &failed_trial);
    assert(rc == 0);
    assert(counts.clean == 31);
    assert(counts.unrelated == 0);

    printf("PASS: reference trial with bad samples counts once as unrelated trial\n");
}

/* Abort path test: on -1, stop immediately; nothing submitted, reset or rewritten after failure */
static void test_abort_immediate_stop(void)
{
    fake_trial_state s;
    memset(&s, 0, sizeof s);
    s.fail_at_trial = 7; /* trial 7 fails */
    s.outcome_to_report = A2P_TRIAL_CLEAN;

    a2p_series_counts counts;
    unsigned failed_trial = 0;

    int rc = a2p_run_series_loop(&s, 0, A2P_POISON_VERTEX, 32, fake_trial_fn, &counts, &failed_trial);
    assert(rc == -1);
    assert(failed_trial == 7);
    /* Only trials 0..7 were called (total 8 calls); trials 8..31 were never called */
    assert(s.calls == 8);
    /* Only trials 0..6 succeeded and performed submit/reset/rewrite */
    assert(s.submitted == 7);
    assert(s.reset == 7);
    assert(s.rewritten == 7);

    printf("PASS: abort path stops immediately at trial 7: no subsequent trials called or rewritten\n");
}

/* Verification that a series loop exhibiting wrong behaviour (continuing after error) is detected and rejected */
static void test_abort_wrong_behaviour_detected(void)
{
    fake_trial_state s_flawed;
    memset(&s_flawed, 0, sizeof s_flawed);
    s_flawed.fail_at_trial = 7;
    s_flawed.outcome_to_report = A2P_TRIAL_CLEAN;

    a2p_series_counts counts_flawed;
    unsigned failed_trial_flawed = 0;

    int flawed_rc = a2p_run_series_loop_flawed_continue(&s_flawed, 0, A2P_POISON_VERTEX, 32,
                                                       fake_trial_fn, &counts_flawed, &failed_trial_flawed);
    /* The flawed loop failed to stop: it returned 0 and executed all 32 trials */
    assert(flawed_rc == 0);
    assert(s_flawed.calls == 32);
    assert(s_flawed.submitted == 31);

    /* The real loop must catch the error and halt at trial 7 with rc == -1 */
    fake_trial_state s_real;
    memset(&s_real, 0, sizeof s_real);
    s_real.fail_at_trial = 7;
    s_real.outcome_to_report = A2P_TRIAL_CLEAN;

    a2p_series_counts counts_real;
    unsigned failed_trial_real = 0;
    int real_rc = a2p_run_series_loop(&s_real, 0, A2P_POISON_VERTEX, 32,
                                     fake_trial_fn, &counts_real, &failed_trial_real);
    assert(real_rc == -1);
    assert(failed_trial_real == 7);
    assert(s_real.calls == 8);
    assert(s_real.submitted == 7);

    printf("PASS: abort test detects and rejects continue-on-failure loop behaviour\n");
}

/* Fake clock trial runner simulating pre-trial, wait loop, and readback deadline checks */
static int fake_trial_clock_fn(void *user_ctx, int mode, int poison_kind, unsigned trial,
                               int *out_trial_outcome, int *out_all_clear)
{
    (void)mode;
    (void)poison_kind;
    (void)trial;
    fake_trial_state *s = (fake_trial_state *)user_ctx;
    s->calls++;

    /* 1. Checkpoint: pre-trial */
    if (s->trigger_checkpoint == 0 && trial == s->trigger_at_trial)
        s->fake_now_us = s->deadline_us;
    if (s->fake_now_us >= s->deadline_us) {
        s->deadline_at = "pre-trial";
        return A2P_RC_DEADLINE;
    }

    s->submitted++;

    /* Advance clock to simulate execution up to completion label wait loop */
    s->fake_now_us += s->step_per_trial_us / 3;

    /* 2. Checkpoint: inside wait loop (done-label-wait) */
    if (s->trigger_checkpoint == 1 && trial == s->trigger_at_trial)
        s->fake_now_us = s->deadline_us;
    if (s->fake_now_us >= s->deadline_us) {
        s->deadline_at = "done-label-wait";
        return A2P_RC_DEADLINE;
    }

    s->reset++;
    s->rewritten++;

    /* Advance clock to simulate execution up to readback */
    s->fake_now_us += s->step_per_trial_us / 3;

    /* 3. Checkpoint: pre-readback */
    if (s->trigger_checkpoint == 2 && trial == s->trigger_at_trial)
        s->fake_now_us = s->deadline_us;
    if (s->fake_now_us >= s->deadline_us) {
        s->deadline_at = "pre-readback";
        return A2P_RC_DEADLINE;
    }

    /* Advance clock to simulate execution inside readback wait */
    s->fake_now_us += s->step_per_trial_us / 3;

    /* 4. Checkpoint: inside readback wait (readback-wait) */
    if (s->trigger_checkpoint == 3 && trial == s->trigger_at_trial)
        s->fake_now_us = s->deadline_us;
    if (s->fake_now_us >= s->deadline_us) {
        s->deadline_at = "readback-wait";
        return A2P_RC_DEADLINE;
    }

    *out_trial_outcome = s->outcome_to_report;
    if (out_all_clear)
        *out_all_clear = s->all_clear_to_report;
    return 0;
}

/* Simulate warm-up loop with cooperative deadline check */
static int simulate_warmup(fake_trial_state *s, unsigned max_warmup, unsigned trip_at_w)
{
    for (unsigned w = 0; w < max_warmup; ++w) {
        if (w == trip_at_w)
            s->fake_now_us = s->deadline_us;
        if (s->fake_now_us >= s->deadline_us) {
            s->deadline_at = "warmup";
            return A2P_RC_DEADLINE;
        }
        int outcome = A2P_TRIAL_CLEAN;
        int all_clear = 0;
        int rc = fake_trial_clock_fn(s, 0, A2P_POISON_NONE, w, &outcome, &all_clear);
        if (rc != 0)
            return rc;
        if (outcome == A2P_TRIAL_CLEAN)
            return 0;
    }
    return 1;
}

/* Deadline test: covers expiry at EACH checkpoint, exact boundaries (limit and limit-1), and per-series reporting */
static void test_abort_deadline_fake_clock(void)
{
    a2p_series_counts counts;
    unsigned failed_trial = 0;

    /* Checkpoint 1: Expiry before a trial (pre-trial at trial 4) */
    fake_trial_state s;
    memset(&s, 0, sizeof s);
    s.outcome_to_report = A2P_TRIAL_CLEAN;
    s.deadline_us = A2P_SESSION_LIMIT_US;
    s.fake_now_us = 0;
    s.step_per_trial_us = 1000000ULL;
    s.trigger_checkpoint = 0;
    s.trigger_at_trial = 4;
    int rc = a2p_run_series_loop(&s, 0, A2P_POISON_VERTEX, 32, fake_trial_clock_fn, &counts, &failed_trial);
    assert(rc == A2P_RC_DEADLINE);
    assert(failed_trial == 4);
    assert(strcmp(s.deadline_at, "pre-trial") == 0);
    assert(s.calls == 5);     /* trials 0..4 called; trials 5..31 never called */
    assert(s.submitted == 4); /* trial 4 halted before submission */
    assert(s.reset == 4);
    assert(s.rewritten == 4);
    assert(counts.clean == 4);

    /* Checkpoint 2: Expiry inside wait loop (done-label-wait at trial 4) */
    memset(&s, 0, sizeof s);
    s.outcome_to_report = A2P_TRIAL_CLEAN;
    s.deadline_us = A2P_SESSION_LIMIT_US;
    s.fake_now_us = 0;
    s.step_per_trial_us = 1000000ULL;
    s.trigger_checkpoint = 1;
    s.trigger_at_trial = 4;
    failed_trial = 0;
    rc = a2p_run_series_loop(&s, 0, A2P_POISON_VERTEX, 32, fake_trial_clock_fn, &counts, &failed_trial);
    assert(rc == A2P_RC_DEADLINE);
    assert(failed_trial == 4);
    assert(strcmp(s.deadline_at, "done-label-wait") == 0);
    assert(s.calls == 5);
    assert(s.submitted == 5); /* trial 4 submitted before wait loop */
    assert(s.reset == 4);     /* halted in wait loop before reset or rewrite */
    assert(s.rewritten == 4);
    assert(counts.clean == 4);

    /* Checkpoint 3: Expiry before readback (pre-readback at trial 4) */
    memset(&s, 0, sizeof s);
    s.outcome_to_report = A2P_TRIAL_CLEAN;
    s.deadline_us = A2P_SESSION_LIMIT_US;
    s.fake_now_us = 0;
    s.step_per_trial_us = 1000000ULL;
    s.trigger_checkpoint = 2;
    s.trigger_at_trial = 4;
    failed_trial = 0;
    rc = a2p_run_series_loop(&s, 0, A2P_POISON_VERTEX, 32, fake_trial_clock_fn, &counts, &failed_trial);
    assert(rc == A2P_RC_DEADLINE);
    assert(failed_trial == 4);
    assert(strcmp(s.deadline_at, "pre-readback") == 0);
    assert(s.calls == 5);
    assert(s.submitted == 5);
    assert(s.reset == 5);
    assert(s.rewritten == 5);
    assert(counts.clean == 4);

    /* Checkpoint 4: Expiry inside readback wait (readback-wait at trial 4) */
    memset(&s, 0, sizeof s);
    s.outcome_to_report = A2P_TRIAL_CLEAN;
    s.deadline_us = A2P_SESSION_LIMIT_US;
    s.fake_now_us = 0;
    s.step_per_trial_us = 1000000ULL;
    s.trigger_checkpoint = 3;
    s.trigger_at_trial = 4;
    failed_trial = 0;
    rc = a2p_run_series_loop(&s, 0, A2P_POISON_VERTEX, 32, fake_trial_clock_fn, &counts, &failed_trial);
    assert(rc == A2P_RC_DEADLINE);
    assert(failed_trial == 4);
    assert(strcmp(s.deadline_at, "readback-wait") == 0);
    assert(s.calls == 5);
    assert(s.submitted == 5);
    assert(s.reset == 5);
    assert(s.rewritten == 5);
    assert(counts.clean == 4);

    /* Checkpoint 5: Expiry during warm-up (at warm-up trial 3) */
    memset(&s, 0, sizeof s);
    s.outcome_to_report = A2P_TRIAL_UNRELATED; /* shader compiling, keep warming up until trip */
    s.deadline_us = A2P_SESSION_LIMIT_US;
    s.fake_now_us = 0;
    s.trigger_checkpoint = -1;
    rc = simulate_warmup(&s, 30, 3);
    assert(rc == A2P_RC_DEADLINE);
    assert(strcmp(s.deadline_at, "warmup") == 0);
    assert(s.calls == 3); /* trials 0, 1, 2 ran; trial 3 never ran */
    assert(s.submitted == 3);

    /* Exact boundary test: elapsed == limit (must expire with code 3) */
    memset(&s, 0, sizeof s);
    s.deadline_us = A2P_SESSION_LIMIT_US;
    s.fake_now_us = A2P_SESSION_LIMIT_US; /* elapsed == limit */
    s.trigger_checkpoint = -1;
    failed_trial = 0;
    rc = a2p_run_series_loop(&s, 0, A2P_POISON_VERTEX, 32, fake_trial_clock_fn, &counts, &failed_trial);
    assert(rc == A2P_RC_DEADLINE);
    assert(failed_trial == 0);
    assert(strcmp(s.deadline_at, "pre-trial") == 0);
    assert(s.calls == 1);
    assert(s.submitted == 0);

    /* Exact boundary test: elapsed == limit - 1 (must NOT expire; succeeds) */
    memset(&s, 0, sizeof s);
    s.outcome_to_report = A2P_TRIAL_CLEAN;
    s.deadline_us = A2P_SESSION_LIMIT_US;
    s.fake_now_us = A2P_SESSION_LIMIT_US - 1; /* limit - 1 */
    s.step_per_trial_us = 0; /* clock frozen at limit - 1 */
    s.trigger_checkpoint = -1;
    failed_trial = 0;
    rc = a2p_run_series_loop(&s, 0, A2P_POISON_VERTEX, 1, fake_trial_clock_fn, &counts, &failed_trial);
    assert(rc == 0);
    assert(counts.clean == 1);
    assert(s.calls == 1);
    assert(s.submitted == 1);
    assert(s.reset == 1);
    assert(s.rewritten == 1);

    /* Elapsed per-series reporting verification */
    char summary[256];
    a2p_format_series_summary(summary, sizeof summary, "backend", "vertex", &counts, 32, 12345678ULL);
    assert(strstr(summary, "A2P mode=backend poison=vertex expected=0/32 unrelated=0/32 clean=1/32 elapsed_us=12345678") != NULL);

    printf("PASS: cooperative monotonic deadline: all checkpoints, exact boundaries (limit and limit-1), and per-series elapsed reporting\n");
}

/* Eight-fetch max semantics test:
 * Fragment program arena_fp combines eight texture fetches with component-wise max().
 * A partially late texture read mixes GOOD and POISON fetches, so the pixel is max(GOOD, POISON)
 * per channel, not pure ff00ff.
 * Requirements:
 *   - all-good -> GOOD
 *   - all-poison -> expected (for texture)
 *   - mixed max(GOOD, POISON) -> expected for texture
 *   - same mixed value under VERTEX and INDEX poison -> unrelated
 *   - other colours -> unrelated
 */
static void test_texture_eight_fetch_max_semantics(void)
{
    /* Concrete case: texture EARLY trial 0, GOOD=0xff3a4760 */
    uint32_t good = 0xff3a4760u;
    uint32_t good_rgb = good & 0x00ffffffu; /* 0x003a4760 */
    uint32_t poison = A2P_COLOR_POISON;     /* 0xffff00ff */
    uint32_t poison_rgb = poison & 0x00ffffffu; /* 0x00ff00ff */
    uint32_t mixed_rgb = a2p_color_max_rgb(good_rgb, poison_rgb); /* 0x00ff47ff */
    assert(mixed_rgb == 0x00ff47ffu);

    /* 1. All-good (all 8 fetches read GOOD) -> GOOD across all modes */
    assert(a2p_classify_sample(good, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_GOOD);
    assert(a2p_classify_sample(good_rgb, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_GOOD);
    assert(a2p_classify_sample(good, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_GOOD);
    assert(a2p_classify_sample(good, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_GOOD);
    assert(a2p_classify_sample(good, good, A2P_POISON_NONE, 1) == A2P_SAMPLE_GOOD);

    /* 2. All-poison (all 8 fetches read POISON) -> expected for texture, unrelated for others */
    assert(a2p_classify_sample(poison, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_EXPECTED);
    assert(a2p_classify_sample(poison_rgb, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_EXPECTED);
    assert(a2p_classify_sample(poison, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(poison, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(poison, good, A2P_POISON_NONE, 1) == A2P_SAMPLE_UNRELATED);

    /* 3. Mixed max(GOOD, POISON) -> expected for texture (simulating 1..7 poison fetches) */
    for (int k = 1; k <= 7; ++k) {
        uint32_t simulated = good_rgb;
        for (int f = 0; f < k; ++f)
            simulated = a2p_color_max_rgb(simulated, poison_rgb);
        assert(simulated == mixed_rgb);
        assert(a2p_classify_sample(simulated, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_EXPECTED);
        assert(a2p_classify_sample(0xff000000u | simulated, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_EXPECTED);
    }

    /* 4. Same mixed value under VERTEX and INDEX poison -> unrelated */
    assert(a2p_classify_sample(mixed_rgb, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(mixed_rgb, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(mixed_rgb, good, A2P_POISON_NONE, 1) == A2P_SAMPLE_UNRELATED);

    /* 5. Other colours -> unrelated across all modes */
    uint32_t unknown1 = 0xff123456u;
    uint32_t unknown2 = 0xff4700ffu; /* subtle variation: R=ff, G=47, B=ff vs B=00 */
    assert(a2p_classify_sample(unknown1, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(unknown1, good, A2P_POISON_VERTEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(unknown1, good, A2P_POISON_INDEX, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(unknown2, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(A2P_SENTINEL, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_UNRELATED);
    assert(a2p_classify_sample(A2P_COLOR_CLEAR, good, A2P_POISON_TEXTURE, 0) == A2P_SAMPLE_UNRELATED);

    printf("PASS: eight-fetch max semantics: all-good, all-poison, mixed expected for texture, unrelated for vertex/index/reference\n");
}

/* Per-tile classification test: 256 tiles, each with 32 distinct sub-quads (8192 sample points) */
static void test_per_tile_classification(void)
{
    size_t total_px = (size_t)A2P_RT_DIM * A2P_RT_DIM;
    uint32_t *buf = (uint32_t *)malloc(total_px * sizeof(uint32_t));
    assert(buf != NULL);

    uint32_t good = 0xff112233u;

    /* Fill all 8192 sub-quad sample points with good colour */
    for (size_t i = 0; i < total_px; ++i)
        buf[i] = A2P_COLOR_CLEAR;

    for (unsigned t = 0; t < A2P_TILES; ++t) {
        for (unsigned q = 0; q < A2P_SUBQUADS_PER_TILE; ++q) {
            size_t off = a2p_subquad_sample_offset(t, q);
            buf[off] = good;
        }
    }

    /* 1. All clean test */
    a2p_tile_report rep;
    int outcome = a2p_classify_trial_tiles(buf, good, A2P_POISON_INDEX, 0, &rep);
    assert(outcome == A2P_TRIAL_CLEAN);
    assert(rep.clean_tile_count == 256);
    assert(rep.expected_tile_count == 0);
    assert(rep.unrelated_tile_count == 0);
    assert(rep.all_clear == 0);

    char mask_str[80];
    a2p_format_bitmask(rep.expected_mask, mask_str, sizeof mask_str);
    assert(strcmp(mask_str, "0000000000000000000000000000000000000000000000000000000000000000") == 0);

    /* 2. Expected corruption in tiles 0, 42, 255 (set sub-quad center to CLEAR under INDEX poison) */
    buf[a2p_subquad_sample_offset(0, 5)] = A2P_COLOR_CLEAR;
    buf[a2p_subquad_sample_offset(42, 12)] = A2P_COLOR_CLEAR;
    buf[a2p_subquad_sample_offset(255, 31)] = A2P_COLOR_CLEAR;

    outcome = a2p_classify_trial_tiles(buf, good, A2P_POISON_INDEX, 0, &rep);
    assert(outcome == A2P_TRIAL_EXPECTED);
    assert(rep.expected_tile_count == 3);
    assert(rep.unrelated_tile_count == 0);
    assert(rep.clean_tile_count == 253);
    assert((rep.expected_mask[0 / 32] & (1u << (0 % 32))) != 0);
    assert((rep.expected_mask[42 / 32] & (1u << (42 % 32))) != 0);
    assert((rep.expected_mask[255 / 32] & (1u << (255 % 32))) != 0);

    /* 3. Unrelated corruption in tile 10 (unknown colour in sub-quad 1) */
    buf[a2p_subquad_sample_offset(10, 1)] = 0xff998877u;
    outcome = a2p_classify_trial_tiles(buf, good, A2P_POISON_INDEX, 0, &rep);
    assert(outcome == A2P_TRIAL_UNRELATED);
    assert(rep.unrelated_tile_count == 1);
    assert(rep.expected_tile_count == 3);
    assert((rep.unrelated_mask[10 / 32] & (1u << (10 % 32))) != 0);

    /* 4. Dual corruption in tile 20: holding BOTH expected and unrelated sub-quad samples */
    buf[a2p_subquad_sample_offset(20, 2)] = A2P_COLOR_CLEAR; /* expected under INDEX */
    buf[a2p_subquad_sample_offset(20, 3)] = 0xff887766u;     /* unrelated unknown colour */
    outcome = a2p_classify_trial_tiles(buf, good, A2P_POISON_INDEX, 0, &rep);
    /* Tile 20 MUST be set in BOTH masks */
    assert((rep.expected_mask[20 / 32] & (1u << (20 % 32))) != 0);
    assert((rep.unrelated_mask[20 / 32] & (1u << (20 % 32))) != 0);
    /* Trial outcome must maintain invalid priority */
    assert(outcome == A2P_TRIAL_UNRELATED);

    free(buf);
    printf("PASS: per-tile classification: clean, expected bitmask, unrelated, and dual-mask tile\n");
}

/* Explicit test: an all-CLEAR completed reference must be classified as INVALID */
static void test_all_clear_reference_invalid(void)
{
    size_t total_px = (size_t)A2P_RT_DIM * A2P_RT_DIM;
    uint32_t *buf = (uint32_t *)malloc(total_px * sizeof(uint32_t));
    assert(buf != NULL);

    /* Buffer is pure A2P_COLOR_CLEAR */
    for (size_t i = 0; i < total_px; ++i)
        buf[i] = A2P_COLOR_CLEAR;

    uint32_t good = 0xff112233u;
    a2p_tile_report rep;

    /* Classify in reference mode (is_reference = 1, poison_kind = A2P_POISON_NONE) */
    int outcome = a2p_classify_trial_tiles(buf, good, A2P_POISON_NONE, 1, &rep);

    /* Assert trial level classification */
    assert(rep.all_clear == 1);
    assert(outcome == A2P_TRIAL_UNRELATED); /* MUST NOT be clean, MUST NOT be expected */
    assert(rep.clean_tile_count == 0);
    assert(rep.unrelated_tile_count == 256);
    for (unsigned w = 0; w < A2P_MASK_WORDS; ++w)
        assert(rep.unrelated_mask[w] == 0xffffffffu);

    /* Simulate 32-trial reference series where every trial was all-clear */
    fake_trial_state s;
    memset(&s, 0, sizeof s);
    s.fail_at_trial = 999;
    s.outcome_to_report = A2P_TRIAL_UNRELATED;
    s.all_clear_to_report = 1;

    a2p_series_counts ref_counts;
    unsigned failed_trial = 0;
    int rc = a2p_run_series_loop(&s, 0, A2P_POISON_NONE, 32, fake_trial_fn, &ref_counts, &failed_trial);
    assert(rc == 0);
    assert(ref_counts.clean == 0);
    assert(ref_counts.unrelated == 32);
    assert(ref_counts.all_clear_trials == 32);

    /* Evaluate verdict for reference series: MUST be classified invalid */
    a2p_series_counts dummy_counts = { .expected = 32, .unrelated = 0, .clean = 0, .all_clear_trials = 0 };
    char reason[128] = {0};
    const char *verdict = a2p_evaluate_verdict(ref_counts, dummy_counts, dummy_counts, dummy_counts,
                                               "backend", NULL, NULL, reason, sizeof reason);
    assert(strcmp(verdict, "invalid") == 0);
    assert(strcmp(reason, "reference all-clear") == 0);

    free(buf);
    printf("PASS: all-CLEAR completed reference classified invalid (asserted explicitly)\n");
}

static void test_deadline_boundary(void)
{
    assert(!a2p_deadline_expired(0));
    assert(!a2p_deadline_expired(A2P_SESSION_LIMIT_US - 1u));
    assert(a2p_deadline_expired(A2P_SESSION_LIMIT_US));
    assert(a2p_deadline_expired(A2P_SESSION_LIMIT_US + 1u));
    assert(a2p_deadline_expired(UINT64_MAX));
    printf("PASS: deadline boundary: limit - 1 continues, limit and beyond expire\n");
}

int main(void)
{
    test_neg_poison_under_vertex();
    test_neg_poison_under_index();
    test_neg_clear_under_texture();
    test_neg_sentinel();
    test_neg_unknown_colour();
    test_pos_expected_controls();
    test_texture_eight_fetch_max_semantics();
    test_reference_counted_once();
    test_abort_immediate_stop();
    test_abort_wrong_behaviour_detected();
    test_abort_deadline_fake_clock();
    test_deadline_boundary();
    test_per_tile_classification();
    test_all_clear_reference_invalid();

    printf("ALL A2P PROBE TESTS PASSED\n");
    return 0;
}
