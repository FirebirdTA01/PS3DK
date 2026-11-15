#ifndef A2P_CLASSIFIER_H
#define A2P_CLASSIFIER_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

#define A2P_RT_DIM              512u
#define A2P_TILES_X             16u
#define A2P_TILES_Y             16u
#define A2P_TILES               (A2P_TILES_X * A2P_TILES_Y) /* 256 tiles */
#define A2P_TILE_CELL_SIZE      (A2P_RT_DIM / A2P_TILES_X)  /* 32 pixels */
#define A2P_MASK_WORDS          (A2P_TILES / 32u)           /* 8 words = 256 bits */

#define A2P_SUBQUADS_X          4u
#define A2P_SUBQUADS_Y          8u
#define A2P_SUBQUADS_PER_TILE   (A2P_SUBQUADS_X * A2P_SUBQUADS_Y) /* 32 sub-quads per tile */
#define A2P_TOTAL_SUBQUADS      (A2P_TILES * A2P_SUBQUADS_PER_TILE) /* 8192 sub-quads */
#define A2P_TOTAL_SAMPLES       A2P_TOTAL_SUBQUADS /* 8192 samples (1 per sub-quad) */

#define A2P_SUBQUAD_W           (A2P_TILE_CELL_SIZE / A2P_SUBQUADS_X) /* 8 pixels */
#define A2P_SUBQUAD_H           (A2P_TILE_CELL_SIZE / A2P_SUBQUADS_Y) /* 4 pixels */

#define A2P_VERTS_PER_SUBQUAD   4u
#define A2P_ON_TARGET_VERTS     (A2P_TOTAL_SUBQUADS * A2P_VERTS_PER_SUBQUAD) /* 32,768 vertices */
#define A2P_BACKLOG_VERTS       4u
#define A2P_OFF_SCREEN_VERTS    4u
#define A2P_TOTAL_VERTS         (A2P_ON_TARGET_VERTS + A2P_BACKLOG_VERTS + A2P_OFF_SCREEN_VERTS) /* 32,776 vertices */

#define A2P_INDICES_PER_SUBQUAD 6u
#define A2P_INDICES_PER_TILE    (A2P_SUBQUADS_PER_TILE * A2P_INDICES_PER_SUBQUAD) /* 192 indices */
#define A2P_TOTAL_INDICES       (A2P_TILES * A2P_INDICES_PER_TILE) /* 49,152 indices */

#define A2P_COLOR_CLEAR         0xff000000u
#define A2P_COLOR_POISON        0xffff00ffu
#define A2P_SENTINEL            0x5a5a5a5au
/* Monotonic session deadline: 180 s wall-clock budget starting at A2P_BEGIN (including setup and warm-up).
 * The bound is cooperative: a blocking GCM callback, flush, or file I/O can delay the next check.
 * Expiry is terminal: stops immediately, preventing any further draws, resets, rewrites, or readbacks.
 */
#define A2P_SESSION_LIMIT_US    (180ULL * 1000000ULL) /* 180 s cooperative session deadline in microseconds */
#define A2P_RC_DEADLINE         3

enum {
    A2P_POISON_NONE = 0,
    A2P_POISON_VERTEX,
    A2P_POISON_INDEX,
    A2P_POISON_TEXTURE
};

/* Convenience aliases for source compatibility */
#define POISON_NONE    A2P_POISON_NONE
#define POISON_VERTEX  A2P_POISON_VERTEX
#define POISON_INDEX   A2P_POISON_INDEX
#define POISON_TEXTURE A2P_POISON_TEXTURE

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
    uint32_t expected_mask[A2P_MASK_WORDS];
    uint32_t unrelated_mask[A2P_MASK_WORDS];
    unsigned expected_tile_count;
    unsigned unrelated_tile_count;
    unsigned clean_tile_count;
    int all_clear; /* 1 if all 8192 sub-quad samples across all tiles were clear */
} a2p_tile_report;

typedef struct {
    unsigned expected;
    unsigned unrelated;
    unsigned clean;
    unsigned all_clear_trials;
} a2p_series_counts;

/* Compute component-wise max in 24-bit RGB space (derived from the 8-fetch fragment shader max() semantics) */
static inline uint32_t a2p_color_max_rgb(uint32_t c1, uint32_t c2)
{
    uint32_t r1 = (c1 >> 16) & 0xffu, g1 = (c1 >> 8) & 0xffu, b1 = c1 & 0xffu;
    uint32_t r2 = (c2 >> 16) & 0xffu, g2 = (c2 >> 8) & 0xffu, b2 = c2 & 0xffu;
    uint32_t rm = r1 > r2 ? r1 : r2;
    uint32_t gm = g1 > g2 ? g1 : g2;
    uint32_t bm = b1 > b2 ? b1 : b2;
    return (rm << 16) | (gm << 8) | bm;
}

/* Classify a single pixel sample.
 * Exact 24-bit RGB comparison (ignoring alpha).
 * Sentinel is always UNRELATED.
 * If rgb == good_rgb: always GOOD.
 * Otherwise, the non-good sample lands in EXACTLY ONE bucket:
 *   - For vertex or index poison (and not reference): A2P_COLOR_CLEAR is EXPECTED, all others UNRELATED.
 *   - For texture poison (and not reference): exact pure A2P_COLOR_POISON and the derived mixed value
 *     (per-channel max(GOOD, POISON) from the 8-fetch fragment shader max() semantics) are EXPECTED,
 *     all others UNRELATED.
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
        uint32_t poison_rgb = A2P_COLOR_POISON & 0x00ffffffu;
        uint32_t mixed_rgb = a2p_color_max_rgb(good_rgb, poison_rgb);
        if (rgb == poison_rgb || rgb == mixed_rgb)
            return A2P_SAMPLE_EXPECTED;
        return A2P_SAMPLE_UNRELATED;
    }

    return A2P_SAMPLE_UNRELATED;
}

/* Sample coordinates for a sub-rectangle within a tile in a 512x512 surface:
 * Tile cell is 32x32 pixels. Sub-quads form a 4x8 grid inside the tile (8x4 pixels each).
 * Center of sub-quad (sx, sy) is at (tile_x + sx*8 + 4, tile_y + sy*4 + 2).
 */
static inline size_t a2p_subquad_sample_offset(unsigned tile_index, unsigned subquad_index)
{
    unsigned tx = tile_index % A2P_TILES_X;
    unsigned ty = tile_index / A2P_TILES_X;
    unsigned sx = subquad_index % A2P_SUBQUADS_X;
    unsigned sy = subquad_index / A2P_SUBQUADS_X;

    unsigned cx = tx * A2P_TILE_CELL_SIZE + sx * A2P_SUBQUAD_W + 4u;
    unsigned cy = ty * A2P_TILE_CELL_SIZE + sy * A2P_SUBQUAD_H + 2u;
    return (size_t)cy * A2P_RT_DIM + (size_t)cx;
}

/* Classify all 256 tiles across the 512x512 surface.
 * Each tile is evaluated over its 32 distinct sub-rectangles (8192 total samples).
 *
 * Tile mask rules:
 *   - A tile holding unrelated samples is set in unrelated_mask.
 *   - A tile holding expected samples is set in expected_mask.
 *   - A tile holding BOTH is set in BOTH masks.
 *   - A tile with neither is clean.
 *
 * Trial outcome logic:
 *   - If is_reference:
 *       Every tile must be clean (all 32 sub-rectangles GOOD).
 *       An all-CLEAR reference or any non-good sub-rectangle returns A2P_TRIAL_UNRELATED.
 *   - If not reference:
 *       Any unrelated sub-rectangle marks the trial as A2P_TRIAL_UNRELATED (invalid priority).
 *       Else if expected sub-rectangles > 0 returns A2P_TRIAL_EXPECTED.
 *       Else returns A2P_TRIAL_CLEAN.
 */
static inline int a2p_classify_trial_tiles(const volatile uint32_t *readback,
                                           uint32_t good,
                                           int poison_kind,
                                           int is_reference,
                                           a2p_tile_report *report)
{
    memset(report, 0, sizeof(*report));

    unsigned total_clear_samples = 0;

    for (unsigned t = 0; t < A2P_TILES; ++t) {
        int tile_unrel = 0;
        int tile_exp = 0;

        for (unsigned q = 0; q < A2P_SUBQUADS_PER_TILE; ++q) {
            size_t off = a2p_subquad_sample_offset(t, q);
            uint32_t pix = readback[off];
            if ((pix & 0x00ffffffu) == (A2P_COLOR_CLEAR & 0x00ffffffu))
                total_clear_samples++;
            int c = a2p_classify_sample(pix, good, poison_kind, is_reference);
            if (c == A2P_SAMPLE_UNRELATED)
                tile_unrel = 1;
            else if (c == A2P_SAMPLE_EXPECTED)
                tile_exp = 1;
        }

        unsigned word = t / 32u;
        uint32_t bit = 1u << (t % 32u);

        /* Set in BOTH masks if both types of corruption exist in the tile */
        if (tile_unrel) {
            report->unrelated_mask[word] |= bit;
            report->unrelated_tile_count++;
        }
        if (tile_exp) {
            report->expected_mask[word] |= bit;
            report->expected_tile_count++;
        }
        if (!tile_unrel && !tile_exp) {
            report->clean_tile_count++;
        }
    }

    if (total_clear_samples == A2P_TOTAL_SAMPLES)
        report->all_clear = 1;

    if (is_reference) {
        if (report->all_clear || report->clean_tile_count != A2P_TILES)
            return A2P_TRIAL_UNRELATED;
        return A2P_TRIAL_CLEAN;
    }

    if (report->unrelated_tile_count > 0)
        return A2P_TRIAL_UNRELATED;
    if (report->expected_tile_count > 0)
        return A2P_TRIAL_EXPECTED;
    return A2P_TRIAL_CLEAN;
}

/* Format 256-bit bitmask as 64 hex characters (8 words x 8 hex digits, word 0 first) */
static inline void a2p_format_bitmask(const uint32_t mask[A2P_MASK_WORDS], char *out, size_t out_len)
{
    if (out_len < 65) {
        if (out_len > 0) out[0] = '\0';
        return;
    }
    snprintf(out, out_len, "%08x%08x%08x%08x%08x%08x%08x%08x",
             (unsigned)mask[0], (unsigned)mask[1], (unsigned)mask[2], (unsigned)mask[3],
             (unsigned)mask[4], (unsigned)mask[5], (unsigned)mask[6], (unsigned)mask[7]);
}

/* Evaluate verdict for a buffer kind given the series counts.
 * Precedence: invalid > refuted > supported-for-this-workload > inconclusive.
 */
static inline const char *a2p_evaluate_verdict(a2p_series_counts ref,
                                               a2p_series_counts early,
                                               a2p_series_counts cmd,
                                               a2p_series_counts cand,
                                               const char *cand_mode_name,
                                               const a2p_series_counts *extra,
                                               const char *extra_mode_name,
                                               char *out_reason,
                                               size_t out_reason_len)
{
    if (out_reason && out_reason_len > 0)
        out_reason[0] = '\0';

    if (ref.all_clear_trials == 32u) {
        if (out_reason) snprintf(out_reason, out_reason_len, "reference all-clear");
        return "invalid";
    }
    if (ref.clean != 32u || ref.expected > 0 || ref.unrelated > 0) {
        if (out_reason) snprintf(out_reason, out_reason_len, "reference corrupt");
        return "invalid";
    }
    if (early.unrelated > 0) {
        if (out_reason) snprintf(out_reason, out_reason_len, "unrelated failure in early");
        return "invalid";
    }
    if (early.expected < 8) {
        if (out_reason) snprintf(out_reason, out_reason_len, "detector not sensitive");
        return "invalid";
    }
    if (cmd.unrelated > 0) {
        if (out_reason) snprintf(out_reason, out_reason_len, "unrelated failure in command");
        return "invalid";
    }
    if (cand.unrelated > 0) {
        if (out_reason) snprintf(out_reason, out_reason_len, "unrelated failure in %s", cand_mode_name);
        return "invalid";
    }
    if (extra && extra->unrelated > 0) {
        if (out_reason) snprintf(out_reason, out_reason_len, "unrelated failure in %s", extra_mode_name);
        return "invalid";
    }
    if (cand.expected > 0) {
        if (out_reason) snprintf(out_reason, out_reason_len, "%s corrupt=%u/32", cand_mode_name, cand.expected);
        return "refuted";
    }
    if (cmd.expected >= 8 && cand.expected == 0) {
        if (out_reason) snprintf(out_reason, out_reason_len, "command control caught %u/32, %s 0/32",
                                 cmd.expected, cand_mode_name);
        return "supported-for-this-workload";
    }
    if (out_reason) snprintf(out_reason, out_reason_len, "command control caught %u/32 (< 8/32 threshold)",
                             cmd.expected);
    return "inconclusive";
}

/* Format the per-series summary line with elapsed cooperative runtime */
static inline int a2p_format_series_summary(char *buf, size_t buflen,
                                            const char *mode_str,
                                            const char *poison_str,
                                            const a2p_series_counts *counts,
                                            unsigned trials,
                                            uint64_t elapsed_us)
{
    return snprintf(buf, buflen,
                    "A2P mode=%s poison=%s expected=%u/%u unrelated=%u/%u clean=%u/%u elapsed_us=%llu",
                    mode_str, poison_str,
                    counts->expected, trials,
                    counts->unrelated, trials,
                    counts->clean, trials,
                    (unsigned long long)elapsed_us);
}

/* Function pointer type for trial runner in series */
typedef int (*a2p_trial_runner_fn)(void *user_ctx, int mode, int poison_kind, unsigned trial,
                                   int *out_trial_outcome, int *out_all_clear);

/* Run series loop with abort-on-error behavior:
 * If trial_fn returns nonzero (failure / timeout):
 *   Immediately stop, record aborted status, and return nonzero without running further trials.
 * Otherwise accumulates expected, unrelated, clean, and all_clear trial counts.
 */
static inline int a2p_run_series_loop(void *user_ctx, int mode, int poison_kind, unsigned trials,
                                      a2p_trial_runner_fn trial_fn,
                                      a2p_series_counts *out_counts,
                                      unsigned *out_failed_trial)
{
    out_counts->expected = 0;
    out_counts->unrelated = 0;
    out_counts->clean = 0;
    out_counts->all_clear_trials = 0;

    for (unsigned t = 0; t < trials; ++t) {
        int outcome = A2P_TRIAL_CLEAN;
        int all_clear = 0;
        int rc = trial_fn(user_ctx, mode, poison_kind, t, &outcome, &all_clear);
        if (rc != 0) {
            if (out_failed_trial)
                *out_failed_trial = t;
            return rc; /* Immediate abort with trial error/deadline code */
        }
        if (all_clear)
            out_counts->all_clear_trials++;
        if (outcome == A2P_TRIAL_EXPECTED)
            out_counts->expected++;
        else if (outcome == A2P_TRIAL_UNRELATED)
            out_counts->unrelated++;
        else
            out_counts->clean++;
    }
    return 0;
}

/* Deliberately flawed series loop: ignores trial errors and continues.
 * Used exclusively by host testing to verify that wrong behaviour (continuing after error) is detected and rejected.
 */
static inline int a2p_run_series_loop_flawed_continue(void *user_ctx, int mode, int poison_kind, unsigned trials,
                                                     a2p_trial_runner_fn trial_fn,
                                                     a2p_series_counts *out_counts,
                                                     unsigned *out_failed_trial)
{
    out_counts->expected = 0;
    out_counts->unrelated = 0;
    out_counts->clean = 0;
    out_counts->all_clear_trials = 0;

    for (unsigned t = 0; t < trials; ++t) {
        int outcome = A2P_TRIAL_CLEAN;
        int all_clear = 0;
        int rc = trial_fn(user_ctx, mode, poison_kind, t, &outcome, &all_clear);
        if (rc != 0) {
            if (out_failed_trial)
                *out_failed_trial = t;
            /* Flawed continue: continues after failure instead of aborting */
            continue;
        }
        if (all_clear)
            out_counts->all_clear_trials++;
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
