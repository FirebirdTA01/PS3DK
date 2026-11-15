#ifndef A2P_CLASSIFIER_H
#define A2P_CLASSIFIER_H

#include <stdint.h>
#include <stddef.h>

#define A2P_COLOR_CLEAR    0xff000000u
#define A2P_COLOR_POISON   0xffff00ffu
#define A2P_SENTINEL       0x5a5a5a5au

enum {
    A2P_POISON_NONE = 0,
    A2P_POISON_VERTEX,
    A2P_POISON_INDEX,
    A2P_POISON_TEXTURE
};

enum {
    A2P_SAMPLE_GOOD = 0,
    A2P_SAMPLE_EXPECTED,
    A2P_SAMPLE_UNRELATED
};

enum {
    A2P_TRIAL_CLEAN = 0,
    A2P_TRIAL_EXPECTED,
    A2P_TRIAL_UNRELATED
};

typedef struct {
    unsigned expected;
    unsigned unrelated;
    unsigned clean;
} a2p_series_counts;

/* Classify a single pixel sample.
 * Exact 24-bit RGB comparison (ignoring alpha).
 * Sentinel is always UNRELATED.
 * If rgb == good_rgb: always GOOD.
 * Otherwise, the non-good sample lands in EXACTLY ONE bucket:
 *   - For vertex or index poison (and not reference): A2P_COLOR_CLEAR is EXPECTED, all others UNRELATED.
 *   - For texture poison (and not reference): A2P_COLOR_POISON is EXPECTED, all others UNRELATED.
 *   - For reference (or POISON_NONE): no poison expected, all non-good are UNRELATED.
 */
static inline int a2p_classify_sample(uint32_t pixel, uint32_t good, int poison_kind, int is_reference)
{
    if (pixel == A2P_SENTINEL)
        return A2P_SAMPLE_UNRELATED;

    uint32_t rgb = pixel & 0x00ffffffu;
    uint32_t good_rgb = good & 0x00ffffffu;
    if (rgb == good_rgb)
        return A2P_SAMPLE_GOOD;

    if (is_reference || poison_kind == A2P_POISON_NONE)
        return A2P_SAMPLE_UNRELATED;

    if (poison_kind == A2P_POISON_VERTEX || poison_kind == A2P_POISON_INDEX) {
        if (rgb == (A2P_COLOR_CLEAR & 0x00ffffffu))
            return A2P_SAMPLE_EXPECTED;
        return A2P_SAMPLE_UNRELATED;
    }

    if (poison_kind == A2P_POISON_TEXTURE) {
        if (rgb == (A2P_COLOR_POISON & 0x00ffffffu))
            return A2P_SAMPLE_EXPECTED;
        return A2P_SAMPLE_UNRELATED;
    }

    return A2P_SAMPLE_UNRELATED;
}

/* Classify all samples of a trial.
 * Counts non-good samples and determines trial outcome:
 * - A2P_TRIAL_CLEAN if all samples are GOOD
 * - A2P_TRIAL_UNRELATED if any sample is UNRELATED
 * - A2P_TRIAL_EXPECTED if expected > 0 and unrelated == 0
 */
static inline int a2p_classify_trial_samples(const volatile uint32_t *samples, size_t count,
                                             uint32_t good, int poison_kind, int is_reference,
                                             unsigned *out_expected_samples,
                                             unsigned *out_unrelated_samples)
{
    unsigned exp_cnt = 0;
    unsigned unrel_cnt = 0;

    for (size_t i = 0; i < count; ++i) {
        int c = a2p_classify_sample(samples[i], good, poison_kind, is_reference);
        if (c == A2P_SAMPLE_EXPECTED)
            exp_cnt++;
        else if (c == A2P_SAMPLE_UNRELATED)
            unrel_cnt++;
    }

    if (out_expected_samples)
        *out_expected_samples = exp_cnt;
    if (out_unrelated_samples)
        *out_unrelated_samples = unrel_cnt;

    if (unrel_cnt > 0)
        return A2P_TRIAL_UNRELATED;
    if (exp_cnt > 0)
        return A2P_TRIAL_EXPECTED;
    return A2P_TRIAL_CLEAN;
}

/* Function pointer type for trial runner in series */
typedef int (*a2p_trial_runner_fn)(void *user_ctx, int mode, int poison_kind, unsigned trial,
                                   int *out_trial_outcome);

/* Run series loop with abort-on-error behavior:
 * If trial_fn returns nonzero (failure / timeout):
 *   Immediately stop, record aborted status, and return nonzero without running further trials.
 * Otherwise accumulates expected, unrelated, and clean trial counts.
 */
static inline int a2p_run_series_loop(void *user_ctx, int mode, int poison_kind, unsigned trials,
                                      a2p_trial_runner_fn trial_fn,
                                      a2p_series_counts *out_counts,
                                      unsigned *out_failed_trial)
{
    out_counts->expected = 0;
    out_counts->unrelated = 0;
    out_counts->clean = 0;

    for (unsigned t = 0; t < trials; ++t) {
        int outcome = A2P_TRIAL_CLEAN;
        int rc = trial_fn(user_ctx, mode, poison_kind, t, &outcome);
        if (rc != 0) {
            if (out_failed_trial)
                *out_failed_trial = t;
            return -1; /* Immediate abort */
        }
        if (outcome == A2P_TRIAL_EXPECTED)
            out_counts->expected++;
        else if (outcome == A2P_TRIAL_UNRELATED)
            out_counts->unrelated++;
        else
            out_counts->clean++;
    }
    return 0;
}

#endif /* A2P_CLASSIFIER_H */
