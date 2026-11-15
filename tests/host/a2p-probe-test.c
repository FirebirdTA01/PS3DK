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
} fake_trial_state;

static int fake_trial_fn(void *user_ctx, int mode, int poison_kind, unsigned trial, int *out_trial_outcome)
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
    return 0;
}

/* Negative test 6: reference trial with multiple bad samples counts as exactly ONE failed trial in series */
static void test_reference_counted_once(void)
{
    uint32_t good = 0xff112233u;
    uint32_t samples[100];
    for (int i = 0; i < 100; ++i) {
        if (i < 50)
            samples[i] = A2P_COLOR_CLEAR;
        else if (i < 70)
            samples[i] = A2P_COLOR_POISON;
        else if (i < 80)
            samples[i] = A2P_SENTINEL;
        else
            samples[i] = good;
    }
    unsigned exp_samples = 0, unrel_samples = 0;
    int trial_outcome = a2p_classify_trial_samples(samples, 100, good, A2P_POISON_NONE, 1,
                                                   &exp_samples, &unrel_samples);
    assert(trial_outcome == A2P_TRIAL_UNRELATED);
    assert(exp_samples == 0);
    assert(unrel_samples == 80);

    /* In a 32-trial series where trial 0 has these 80 bad samples and trials 1..31 are clean */
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

    printf("PASS: reference trial with 80 non-good samples counts once as unrelated trial\n");
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

int main(void)
{
    test_neg_poison_under_vertex();
    test_neg_poison_under_index();
    test_neg_clear_under_texture();
    test_neg_sentinel();
    test_neg_unknown_colour();
    test_pos_expected_controls();
    test_reference_counted_once();
    test_abort_immediate_stop();

    printf("ALL A2P PROBE TESTS PASSED\n");
    return 0;
}
