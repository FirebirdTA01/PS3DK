/* cellgcm-transfer-data: dedicated regression probe for cellGcmSetTransferData /
 * rsxSetTransferData M2MF (memory-to-memory-format) 2D transfer completion.
 *
 * Checks:
 *   - 1 MiB LV2 host buffer allocated and mapped for RSX IO via cellGcmMapMainMemory.
 *   - Disjoint local memory range reserved and bounds-checked.
 *   - Nonuniform deterministic payload pattern generated in local source memory.
 *   - Host destination region and adjacent front/back 1024-byte guards poisoned with 0xAA.
 *   - cellGcmSetTransferData invoked with padded pitch (128 bytes width, 192 bytes pitch, 112 rows = 14336 bytes payload).
 *   - Back-end label written behind transfer on dedicated label index 250.
 *   - FIFO flushed and label polled under progress-aware stagnation bound K.
 *   - Synchronized memory inspection using shared C judge:
 *       * Front guard (1024 bytes) uncorrupted (all 0xAA).
 *       * Payload bytes match source pattern byte-for-byte.
 *       * Row padding (64 bytes per row) uncorrupted (all 0xAA).
 *       * Back guard (1024 bytes) uncorrupted (all 0xAA).
 *   - Prints TRANSFER_DATA_OK on full verification, or TRANSFER_DATA_FAIL on any error.
 *
 * NAMED ASSUMPTIONS & CONTRACT:
 * 1. Cross-engine ordering between the RSX M2MF 2D transfer engine and the backend
 *    label engine is an UNPROVEN PREMISE, including under RPCS3 until verified.
 *    Backend label observation proves only that the label command retired in the backend;
 *    it does NOT prove that prior M2MF asynchronous memory writes have committed.
 * 2. Correctness is based on the observed byte/guard result under the named ordering
 *    premise; a one-time matching snapshot does not prove that future GPU writes have ceased.
 * 3. Any sampler can miss intervening hardware progress, so observed progress is tracked.
 *    Polling tracks observed label and FIFO get advancement with a bounded stagnation
 *    counter K. Stagnation terminates as TRANSFER_DATA_INCONCLUSIVE (inconclusive/invalid:
 *    observed unchanged label/get values, not proof that the M2MF engine has ceased).
 * 4. Process exit: returns from main without explicit cellGcmUnmapMainMemory or host
 *    buffer free. Note: this establishes only that no explicit free/unmap is issued prior
 *    to process exit; it does not establish buffer persistence across or after process
 *    termination, nor does it guarantee the safety of outstanding GPU work.
 * 5. External harness watchdog expiration is classified as HARNESS-TIMEOUT (invalid test
 *    run, never a code/emitter verdict).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/process.h>
#include <sys/memory.h>
#include <sys/sys_time.h>
#include <sys/timer.h>
#include <cell/gcm.h>
#include <cell/error.h>

#include "../../../samples/gcm/common/gcm_host_buffer.h"
#include "judge.h"

SYS_PROCESS_PARAM(1001, 0x10000);

#define CB_SIZE             0x100000u               /* 1 MiB command buffer */
#define HOST_GCM_SIZE       (32u * 1024u * 1024u)   /* 32 MiB GCM host storage */
#define TEST_HOST_SIZE      (1024u * 1024u)         /* 1 MiB host allocation for test region */
#define TEST_LOCAL_OFFSET   0x100000u               /* Disjoint local memory offset (1 MiB) */
#define MAX_STAGNANT_POLLS  1000000u                /* Bounded stagnation limit K (polls with 0 observed progress) */

static uint32_t target_get_fifo_progress(void *user_data)
{
    (void)user_data;
    gcmControlRegister volatile *ctrl = (gcmControlRegister volatile *)cellGcmGetControlRegister();
    return ctrl ? (uint32_t)ctrl->get : 0;
}

static void target_sleep_us(uint32_t us, void *user_data)
{
    (void)user_data;
    sys_timer_usleep(us);
}

int main(void)
{
    printf("cellgcm-transfer-data: probe starting\n");

    /* 1. Allocate GCM host buffer and initialize GCM */
    sys_addr_t gcm_host = 0;
    if (gcm_host_buffer_alloc(HOST_GCM_SIZE, &gcm_host) != CELL_OK) {
        printf("TRANSFER_DATA_FAIL gcm_host_buffer_alloc failed for GCM context\n");
        return 1;
    }
    if (cellGcmInit(CB_SIZE, HOST_GCM_SIZE, (void *)(uintptr_t)gcm_host) != CELL_OK) {
        printf("TRANSFER_DATA_FAIL cellGcmInit failed\n");
        return 1;
    }

    CellGcmContextData *ctx = gCellGcmCurrentContext;

    /* 2. Validate local memory layout and reserve disjoint range.
     * In this standalone probe, no other local-memory user exists,
     * ensuring exclusive access to [TEST_LOCAL_OFFSET, TEST_LOCAL_OFFSET + span). */
    CellGcmConfig cfg;
    cellGcmGetConfiguration(&cfg);
    uint32_t src_required = TEST_LOCAL_OFFSET + (TEST_SRC_PITCH * TEST_HEIGHT);
    if (src_required > cfg.localSize) {
        printf("TRANSFER_DATA_FAIL local memory size insufficient (required %u, available %u)\n",
               (unsigned)src_required, (unsigned)cfg.localSize);
        return 1;
    }
    uint8_t *src_ptr = (uint8_t *)cfg.localAddress + TEST_LOCAL_OFFSET;

    /* 3. Allocate 1-MiB-aligned host buffer for test region & map for RSX IO */
    sys_addr_t test_host_addr = 0;
    if (gcm_host_buffer_alloc(TEST_HOST_SIZE, &test_host_addr) != CELL_OK) {
        printf("TRANSFER_DATA_FAIL gcm_host_buffer_alloc failed for test region\n");
        return 1;
    }
    uint32_t test_io_offset = 0;
    int map_rc = cellGcmMapMainMemory((const void *)(uintptr_t)test_host_addr, TEST_HOST_SIZE, &test_io_offset);
    if (map_rc != CELL_OK) {
        printf("TRANSFER_DATA_FAIL cellGcmMapMainMemory failed (rc=0x%08x)\n", (unsigned)map_rc);
        return 1;
    }

    /* 4. Prepare host buffer layout: front guard + test region + back guard */
    uint32_t total_span = GUARD_BAND_SIZE + TEST_REGION_SIZE + GUARD_BAND_SIZE;
    uint32_t dst_offset = 0;
    if (validate_io_mapping(test_io_offset, TEST_HOST_SIZE, GUARD_BAND_SIZE, TEST_REGION_SIZE, &dst_offset) != 0) {
        printf("TRANSFER_DATA_FAIL mapped IO window or destination span arithmetic invalid/overflow\n");
        return 1;
    }

    uint8_t *dst_base = (uint8_t *)(uintptr_t)test_host_addr;
    memset(dst_base, POISON_BYTE, total_span);

    /* Populate source payload with nonuniform pattern */
    for (uint32_t r = 0; r < TEST_HEIGHT; r++) {
        for (uint32_t c = 0; c < TEST_WIDTH; c++) {
            src_ptr[r * TEST_SRC_PITCH + c] = test_pattern_byte(r, c);
        }
    }

    /* 5. Initialize dedicated backend label with explicit NULL check */
    volatile uint32_t *label = cellGcmGetLabelAddress(LABEL_INDEX);
    if (!label) {
        printf("TRANSFER_DATA_FAIL cellGcmGetLabelAddress returned NULL\n");
        return 1;
    }
    *label = LABEL_VAL_INIT;

    /* Enforce memory ordering before command submission */
    __asm__ volatile("sync; isync" ::: "memory");

    /* 6. Emit cellGcmSetTransferData (void return, 8 arguments including ctx) */
    cellGcmSetTransferData(ctx,
                           CELL_GCM_TRANSFER_LOCAL_TO_MAIN,
                           dst_offset,
                           TEST_DST_PITCH,
                           TEST_LOCAL_OFFSET,
                           TEST_SRC_PITCH,
                           TEST_WIDTH,
                           TEST_HEIGHT);

    /* 7. Write backend label behind transfer and flush FIFO */
    cellGcmSetWriteBackEndLabel(ctx, LABEL_INDEX, LABEL_VAL_TARGET);
    cellGcmFlush(ctx);

    /* 8. Poll label under progress-aware stagnation bound K */
    poll_outcome_t outcome = poll_label_progress(label,
                                                 LABEL_VAL_TARGET,
                                                 target_get_fifo_progress,
                                                 target_sleep_us,
                                                 10,                  /* 10 us sleep per check */
                                                 MAX_STAGNANT_POLLS,  /* K = 1,000,000 stagnant checks */
                                                 NULL);
    if (outcome == POLL_NO_PROGRESS) {
        printf("TRANSFER_DATA_INCONCLUSIVE: observed unchanged label/get values for %u polls; not proof that M2MF engine has ceased (label=0x%08x expected=0x%08x)\n",
               (unsigned)MAX_STAGNANT_POLLS, (unsigned)*label, (unsigned)LABEL_VAL_TARGET);
        /* Buffer lifetime note: returns without explicit free/unmap; does not guarantee post-exit safety */
        return 2;
    } else if (outcome != POLL_SUCCESS) {
        printf("TRANSFER_DATA_FAIL poll failed (outcome=%d)\n", (int)outcome);
        return 1;
    }
    printf("cellgcm-transfer-data: label observed at target value 0x%08x\n", (unsigned)LABEL_VAL_TARGET);

    /* Enforce memory barrier before host inspection */
    __asm__ volatile("sync; isync" ::: "memory");

    /* 9. Comprehensive byte judge */
    char reason[256];
    int judge_rc = judge_transfer_bytes(dst_base, GUARD_BAND_SIZE, TEST_WIDTH, TEST_HEIGHT,
                                        TEST_DST_PITCH, POISON_BYTE, reason, sizeof(reason));
    if (judge_rc != 0) {
        printf("TRANSFER_DATA_FAIL %s\n", reason);
        return 1;
    }

    printf("TRANSFER_DATA_OK\n");
    return 0;
}
