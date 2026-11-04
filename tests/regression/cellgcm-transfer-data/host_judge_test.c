#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "judge.h"

#define MAX_LABEL_STEPS 8

typedef struct {
    int step;
    uint32_t val;
} label_schedule_t;

typedef struct {
    int current_step;
    volatile uint32_t *label_ptr;
    label_schedule_t label_schedule[MAX_LABEL_STEPS];
    int label_schedule_count;
    uint32_t fifo_get_val;
    int fifo_advance_every_n;
} mock_poll_env_t;

static uint32_t mock_get_fifo_progress(void *user_data)
{
    mock_poll_env_t *env = (mock_poll_env_t *)user_data;
    if (!env) return 0;
    return env->fifo_get_val;
}

static void mock_sleep_us(uint32_t us, void *user_data)
{
    (void)us;
    mock_poll_env_t *env = (mock_poll_env_t *)user_data;
    if (!env) return;
    env->current_step++;

    /* Periodic FIFO get advancement */
    if (env->fifo_advance_every_n > 0 && (env->current_step % env->fifo_advance_every_n) == 0) {
        env->fifo_get_val += 0x10;
    }

    /* Scheduled label updates */
    if (env->label_ptr) {
        for (int i = 0; i < env->label_schedule_count; i++) {
            if (env->current_step == env->label_schedule[i].step) {
                *env->label_ptr = env->label_schedule[i].val;
                break;
            }
        }
    }
}

static void populate_pristine(uint8_t *buf, size_t total_size)
{
    memset(buf, POISON_BYTE, total_size);
    uint8_t *dst_region = buf + GUARD_BAND_SIZE;
    for (uint32_t r = 0; r < TEST_HEIGHT; r++) {
        uint8_t *row_ptr = dst_region + (r * TEST_DST_PITCH);
        for (uint32_t c = 0; c < TEST_WIDTH; c++) {
            row_ptr[c] = test_pattern_byte(r, c);
        }
    }
}

int main(void)
{
    size_t total_size = GUARD_BAND_SIZE + TEST_REGION_SIZE + GUARD_BAND_SIZE;
    uint8_t *buf = (uint8_t *)malloc(total_size);
    if (!buf) {
        fprintf(stderr, "malloc failed\n");
        return 1;
    }

    char reason[256];
    int failures = 0;

    /* Control 1: Pristine valid transfer buffer */
    populate_pristine(buf, total_size);
    int rc = judge_transfer_bytes(buf, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                                  TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (rc != 0) {
        fprintf(stderr, "FAIL Control 1: pristine buffer rejected (%s)\n", reason);
        failures++;
    } else {
        printf("CONTROL 1 PASS: pristine buffer accepted (TRANSFER_DATA_OK)\n");
    }

    /* Control 2: Untouched poisoned buffer (byte rejection verification) */
    memset(buf, POISON_BYTE, total_size);
    rc = judge_transfer_bytes(buf, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                              TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (rc != 2 || !strstr(reason, "payload mismatch at row 0 col 0")) {
        fprintf(stderr, "FAIL Control 2: untouched buffer byte rejection failed (%d: %s)\n", rc, reason);
        failures++;
    } else {
        printf("CONTROL 2 PASS: untouched buffer byte rejection verified with named reason '%s'\n", reason);
    }

    /* Control 3: Bit-flip in payload region (row 42, col 73) */
    populate_pristine(buf, total_size);
    buf[GUARD_BAND_SIZE + 42 * TEST_DST_PITCH + 73] ^= 0x01;
    rc = judge_transfer_bytes(buf, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                              TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (rc != 2 || !strstr(reason, "payload mismatch at row 42 col 73")) {
        fprintf(stderr, "FAIL Control 3: payload bit-flip not rejected with row 42 col 73 mismatch (%d: %s)\n", rc, reason);
        failures++;
    } else {
        printf("CONTROL 3 PASS: payload bit-flip rejected with named reason '%s'\n", reason);
    }

    /* Control 4: Row padding corruption (row 17, col 130) */
    populate_pristine(buf, total_size);
    buf[GUARD_BAND_SIZE + 17 * TEST_DST_PITCH + 130] = 0x55;
    rc = judge_transfer_bytes(buf, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                              TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (rc != 3 || !strstr(reason, "padding corrupted at row 17 col 130")) {
        fprintf(stderr, "FAIL Control 4: row padding corruption not rejected (%d: %s)\n", rc, reason);
        failures++;
    } else {
        printf("CONTROL 4 PASS: row padding corruption rejected with named reason '%s'\n", reason);
    }

    /* Control 5: Front guard band corruption (offset 512) */
    populate_pristine(buf, total_size);
    buf[512] = 0x00;
    rc = judge_transfer_bytes(buf, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                              TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (rc != 1 || !strstr(reason, "front guard corrupted at offset 512")) {
        fprintf(stderr, "FAIL Control 5: front guard corruption not rejected (%d: %s)\n", rc, reason);
        failures++;
    } else {
        printf("CONTROL 5 PASS: front guard corruption rejected with named reason '%s'\n", reason);
    }

    /* Control 6: Back guard band corruption (offset 100) */
    populate_pristine(buf, total_size);
    buf[GUARD_BAND_SIZE + TEST_REGION_SIZE + 100] = 0xFF;
    rc = judge_transfer_bytes(buf, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                              TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (rc != 4 || !strstr(reason, "back guard corrupted at offset 100")) {
        fprintf(stderr, "FAIL Control 6: back guard corruption not rejected (%d: %s)\n", rc, reason);
        failures++;
    } else {
        printf("CONTROL 6 PASS: back guard corruption rejected with named reason '%s'\n", reason);
    }

    /* Control 7: NULL destination buffer pointer */
    rc = judge_transfer_bytes(NULL, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                              TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (rc != -1 || strcmp(reason, "null destination buffer pointer") != 0) {
        fprintf(stderr, "FAIL Control 7: NULL buffer pointer not rejected with code -1 (%d: %s)\n", rc, reason);
        failures++;
    } else {
        printf("CONTROL 7 PASS: NULL buffer pointer correctly rejected with code -1\n");
    }

    /* Control 8: Shared IO mapping arithmetic and 64-bit overflow validation */
    uint32_t out_dst = 0;
    /* 8a: Normal valid mapping */
    if (validate_io_mapping(0x10000000u, 0x100000u, GUARD_BAND_SIZE, TEST_REGION_SIZE, &out_dst) != 0 ||
        out_dst != 0x10000400u) {
        fprintf(stderr, "FAIL Control 8a: valid mapping rejected\n");
        failures++;
    }
    /* 8b: Wrapping 32-bit overflow at base */
    if (validate_io_mapping(0xFFFFFFF0u, 0x100000u, GUARD_BAND_SIZE, TEST_REGION_SIZE, &out_dst) == 0) {
        fprintf(stderr, "FAIL Control 8b: wrapping 32-bit base overflow accepted\n");
        failures++;
    }
    /* 8c: Upper boundary overflow */
    if (validate_io_mapping(0xFFF00000u, 0x100000u, GUARD_BAND_SIZE, TEST_REGION_SIZE, &out_dst) == 0) {
        fprintf(stderr, "FAIL Control 8c: upper boundary overflow accepted\n");
        failures++;
    }
    /* 8d: Max valid upper boundary */
    if (validate_io_mapping(0xFFEFF000u, 0x100000u, GUARD_BAND_SIZE, TEST_REGION_SIZE, &out_dst) != 0) {
        fprintf(stderr, "FAIL Control 8d: max valid boundary rejected\n");
        failures++;
    }
    /* 8e: Span exceeds host buffer */
    if (validate_io_mapping(0x10000000u, 10000u, GUARD_BAND_SIZE, TEST_REGION_SIZE, &out_dst) == 0) {
        fprintf(stderr, "FAIL Control 8e: span exceeding host buffer accepted\n");
        failures++;
    }
    printf("CONTROL 8 PASS: IO mapping arithmetic boundary and overflow controls verified\n");

    /* Control 9: Progress-aware polling contract */
    /* 9a: NULL label rejection */
    if (poll_label_progress(NULL, LABEL_VAL_TARGET, NULL, NULL, 0, 100, NULL) != POLL_INVALID_ARG) {
        fprintf(stderr, "FAIL Control 9a: NULL label accepted\n");
        failures++;
    }

    /* 9b: Zero max_stagnant_polls rejection */
    volatile uint32_t dummy_lbl = LABEL_VAL_INIT;
    if (poll_label_progress(&dummy_lbl, LABEL_VAL_TARGET, NULL, NULL, 0, 0, NULL) != POLL_INVALID_ARG) {
        fprintf(stderr, "FAIL Control 9b: zero max_stagnant_polls accepted\n");
        failures++;
    }

    /* 9c: Immediate success when target already present (0 steps) */
    volatile uint32_t lbl_imm = LABEL_VAL_TARGET;
    if (poll_label_progress(&lbl_imm, LABEL_VAL_TARGET, NULL, NULL, 0, 10, NULL) != POLL_SUCCESS) {
        fprintf(stderr, "FAIL Control 9c: immediate target match returned error\n");
        failures++;
    }

    /* 9d: Reset of stagnation counter on non-target label changes:
     * max_stagnant = 10, total steps = 20 > 10.
     * No FIFO progress (fifo_advance_every_n = 0).
     * Non-target label updates occur at steps 4, 8, 12, 16.
     * Target reached at step 20.
     * Without label-progress reset, this would fail with POLL_NO_PROGRESS at step 10.
     * With label-progress reset, each non-target update resets stagnation count -> POLL_SUCCESS! */
    volatile uint32_t lbl_step = LABEL_VAL_INIT;
    mock_poll_env_t env_step = {
        .current_step = 0,
        .label_ptr = &lbl_step,
        .label_schedule = {
            { 4, 0xAA01u },
            { 8, 0xAA02u },
            { 12, 0xAA03u },
            { 16, 0xAA04u },
            { 20, LABEL_VAL_TARGET }
        },
        .label_schedule_count = 5,
        .fifo_get_val = 0x100,
        .fifo_advance_every_n = 0
    };
    if (poll_label_progress(&lbl_step, LABEL_VAL_TARGET, mock_get_fifo_progress,
                            mock_sleep_us, 1, 10, &env_step) != POLL_SUCCESS) {
        fprintf(stderr, "FAIL Control 9d: non-target label progress failed to reset stagnation counter\n");
        failures++;
    }

    /* 9e: Reset of stagnation counter on FIFO get advancement:
     * max_stagnant = 10, total steps = 20 > 10.
     * No label updates until target at step 20.
     * FIFO advances every 4 steps (steps 4, 8, 12, 16), resetting stagnation counter.
     * Target set at step 20 -> returns POLL_SUCCESS! */
    volatile uint32_t lbl_reset = LABEL_VAL_INIT;
    mock_poll_env_t env_reset = {
        .current_step = 0,
        .label_ptr = &lbl_reset,
        .label_schedule = {
            { 20, LABEL_VAL_TARGET }
        },
        .label_schedule_count = 1,
        .fifo_get_val = 0x100,
        .fifo_advance_every_n = 4
    };
    if (poll_label_progress(&lbl_reset, LABEL_VAL_TARGET, mock_get_fifo_progress,
                            mock_sleep_us, 1, 10, &env_reset) != POLL_SUCCESS) {
        fprintf(stderr, "FAIL Control 9e: FIFO progress-reset polling failed to complete\n");
        failures++;
    }

    /* 9f: Stagnation failure without progress (max_stagnant=10, no progress) */
    volatile uint32_t lbl_stag = LABEL_VAL_INIT;
    mock_poll_env_t env_stag = {
        .current_step = 0,
        .label_ptr = &lbl_stag,
        .label_schedule_count = 0,
        .fifo_get_val = 0x100,
        .fifo_advance_every_n = 0
    };
    if (poll_label_progress(&lbl_stag, LABEL_VAL_TARGET, mock_get_fifo_progress,
                            mock_sleep_us, 1, 10, &env_stag) != POLL_NO_PROGRESS) {
        fprintf(stderr, "FAIL Control 9f: stagnant polling did not return POLL_NO_PROGRESS\n");
        failures++;
    }
    printf("CONTROL 9 PASS: progress-aware polling contract (null, zero-bound, immediate, label-progress, fifo-progress, stagnation) verified\n");

    free(buf);
    if (failures != 0) {
        fprintf(stderr, "TOTAL FAILURES: %d\n", failures);
        return 1;
    }

    printf("ALL HOST JUDGE CONTROLS PASSED\n");
    return 0;
}
