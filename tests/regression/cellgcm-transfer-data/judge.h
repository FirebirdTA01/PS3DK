#ifndef CELLGCM_TRANSFER_DATA_JUDGE_H
#define CELLGCM_TRANSFER_DATA_JUDGE_H

#include <stdint.h>
#include <stddef.h>

#define GUARD_BAND_SIZE   1024u                   /* 1024-byte front and back guard bands */
#define TEST_WIDTH        128u                    /* 128 bytes width */
#define TEST_HEIGHT       112u                    /* 112 rows */
#define TEST_SRC_PITCH    128u                    /* Packed source pitch (128 bytes) */
#define TEST_DST_PITCH    192u                    /* Padded destination pitch (128 payload + 64 pad) */
#define TEST_REGION_SIZE  (TEST_DST_PITCH * TEST_HEIGHT) /* 192 * 112 = 21504 bytes */
#define POISON_BYTE       0xAAu
#define LABEL_INDEX       250u                    /* Dedicated standalone probe label index */
#define LABEL_VAL_INIT    0x11223344u
#define LABEL_VAL_TARGET  0x55667788u

#ifdef __cplusplus
extern "C" {
#endif

/* Generate deterministic nonuniform pattern byte distinct from POISON_BYTE */
uint8_t test_pattern_byte(uint32_t r, uint32_t c);

/* Comprehensive byte comparison:
 * Checks front guard, payload bytes, row padding bytes, and back guard.
 * Returns 0 on success.
 * On failure, returns non-zero and formats detailed reason into reason_buf. */
int judge_transfer_bytes(const uint8_t *dst_base,
                         uint32_t guard_size,
                         uint32_t width,
                         uint32_t height,
                         uint32_t dst_pitch,
                         uint8_t poison_byte,
                         char *reason_buf,
                         size_t reason_buf_size);

/* Validate RSX IO mapping bounds with 64-bit widening before addition.
 * Checks that:
 * 1. (uint64_t)io_offset + host_size does not exceed UINT32_MAX.
 * 2. total span = guard_size + region_size + guard_size does not exceed host_size.
 * 3. destination start = (uint64_t)io_offset + guard_size does not exceed UINT32_MAX.
 * 4. destination end = (uint64_t)dst_start + region_size does not exceed (uint64_t)io_offset + host_size.
 * Returns 0 on valid, -1 if any bound/overflow is violated.
 * On success, outputs dst_offset into *out_dst_offset. */
int validate_io_mapping(uint32_t io_offset,
                        uint32_t host_size,
                        uint32_t guard_size,
                        uint32_t region_size,
                        uint32_t *out_dst_offset);

/* Polling outcome status */
typedef enum {
    POLL_SUCCESS = 0,          /* Target value observed */
    POLL_NO_PROGRESS = 1,       /* Stagnation bound K reached with zero progress */
    POLL_INVALID_ARG = 2        /* NULL pointer or invalid arguments */
} poll_outcome_t;

/* Callback types for progress inspection and sleeping */
typedef uint32_t (*get_progress_val_fn)(void *user_data);
typedef void (*sleep_us_fn)(uint32_t us, void *user_data);

/* Progress-aware label polling:
 * Repeatedly inspects *label:
 * - If *label == target_val, returns POLL_SUCCESS.
 * - In each iteration, checks if *label changed or if get_fifo_progress() advanced.
 * - If any progress is observed, stagnation counter resets to 0.
 * - If no progress is observed, stagnation counter increments.
 * - If stagnation counter reaches max_stagnant_polls (K), returns POLL_NO_PROGRESS.
 * - If sleep_us callback provided, calls sleep_us(interval_us, user_data).
 *
 * Contract:
 * - Progress is strictly state-based (label update or FIFO get advancement), NEVER elapsed wall time.
 * - Each observed label/FIFO change resets the counter; K consecutive unchanged samples return NO_PROGRESS,
 *   an inconclusive observation. This cannot guarantee detecting all intervening hardware progress.
 * - Failure to observe target after K stagnant polls yields POLL_NO_PROGRESS,
 *   which the caller records as stagnation without mutating or unmapping buffers. */
poll_outcome_t poll_label_progress(volatile const uint32_t *label,
                                   uint32_t target_val,
                                   get_progress_val_fn get_fifo_progress,
                                   sleep_us_fn sleep_us,
                                   uint32_t interval_us,
                                   uint32_t max_stagnant_polls,
                                   void *user_data);

#ifdef __cplusplus
}
#endif

#endif /* CELLGCM_TRANSFER_DATA_JUDGE_H */
