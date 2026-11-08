/* overhead-bench: the seeded block schedule (probe-design-r1 11.2 A-2 with probe-design-r2 D7).  Portable C.
 *
 * For every scheduled case and every TU (ON, OFF, OFF-b: each case has both controls) the schedule holds one
 * UNTIMED warmup block and R timed blocks (R = OB_R = 200), except P-cold, which holds its 64 predeclared fresh sites
 * per TU, each executed exactly once, never warmed and never repeated (D7).  Order: every warmup first, in a seeded
 * permutation; then every timed block and every cold-site execution in ONE seeded permutation, so the blocks of all
 * TUs of a case are interleaved.  The seed and an order digest are printed in the CSV header (ob_sched_header).
 * The per-pair denominator of a (case, TU) is (valid timed blocks) x n; P-empty (n = 0) is a raw block baseline,
 * reported as block elapsed only, never divided; P-cold's denominator is 64 (per site). */
#ifndef OB_SCHED_H
#define OB_SCHED_H

#include <stdint.h>

#define OB_R 200u
#define OB_COLD_SITES 64u

enum { OB_TU_ON = 0, OB_TU_OFF = 1, OB_TU_OFFB = 2, OB_TUS = 3 };
enum { OB_BLK_WARM = 1, OB_BLK_TIMED = 2, OB_BLK_COLD = 3 };

typedef struct ob_block {
    uint16_t c;      /* case index */
    uint8_t tu, kind;
    uint16_t rep;    /* timed: 0..R-1; cold: the site 0..63; warm: 0 */
} ob_block;

/* xorshift64*: the documented generator of the permutation (seed 0 is replaced by a fixed nonzero constant) */
uint64_t ob_rng(uint64_t *state);
/* cases[i] is a case index; cold[i] nonzero marks P-cold.  Writes at most cap blocks; returns the count, or -1 when
 * cap is too small (nothing partial is used). */
long ob_schedule(uint64_t seed, const uint16_t *cases, const int *cold, unsigned ncases, unsigned r, ob_block *out,
                 unsigned long cap);
/* the order digest (FNV-1a 64 over every block's fields, in order) */
uint64_t ob_sched_digest(const ob_block *b, unsigned long n);

/* The GPU section (Plan A r2 4.2 G rows): c is a G case below, tu 0.  One untimed G-g1 warmup first (it registers
 * the GPU zone descriptor), then R timed blocks of each G case in one seeded permutation (generator state
 * seed ^ OB_GPU_SALT).  Returns 1 + 5R, or -1 when cap is too small. */
enum { OB_G_G0 = 0, OB_G_G1 = 1, OB_G_G2 = 2, OB_G_CAL_IDLE = 3, OB_G_CAL_QUEUED = 4, OB_G_CASES = 5 };
#define OB_GPU_SALT 0x4F42475055534544ull
long ob_gpu_schedule(uint64_t seed, unsigned r, ob_block *out, unsigned long cap);

#endif
