/* hello-event-flag-spu — PPU master.
 *
 * Exercises the LV2 event-flag API end-to-end:
 *   - Create a 64-bit event flag (initially zero).
 *   - Spawn NUM_PPU_WORKERS PPU worker threads, each of which sets a
 *     distinct low-numbered bit and exits.
 *   - Spawn a single SPU worker (thread group of one) and instruct it
 *     via signal-notification channels to set bit 6.
 *   - sys_event_flag_wait() for the OR of all expected bits.
 *   - Send the SPU worker SPU_REQ_OP_QUIT, join the group, tear down.
 *
 * Coverage: sys_event_flag_{create,wait,destroy,set}, sysThread{Create,Join},
 * sysSpu{Initialize,ImageImport,ThreadGroupCreate,ThreadInitialize,
 * ThreadGroupStart,ThreadGroupJoin,ThreadWriteSignal}, plus the SPU-side
 * libsputhread surface (sys_event_flag_set_bit{,_impatient},
 * sys_spu_thread_exit, sys_spu_thread_group_exit).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/thread.h>
#include <sys/spu.h>
#include <sys/synchronization.h>

#include "spu_ef.h"
#include "spu_ef_bin.h"

#define MASTER_PRIORITY      1000
#define WORKER_PRIORITY      1500
#define WORKER_STACK         (16 * 1024)

#define SPU_GROUP_PRIORITY   100
#define SPU_GROUP_NAME       "ef_spu_group"
#define SPU_THREAD_NAME      "ef_spu_worker"

#define ptr2ea(p)            ((u64)(uintptr_t)(p))

/* PPU worker thread context — one allocated per worker. */
typedef struct ppu_worker_ctx {
    sys_event_flag_t ef;
    unsigned         idx;
} ppu_worker_ctx_t;

static void ppu_worker_main(void *raw_ctx)
{
    ppu_worker_ctx_t *ctx = (ppu_worker_ctx_t *)raw_ctx;

    printf("Worker[%u] is starting.\n", ctx->idx);

    /* sys_event_flag_set ORs the supplied bit pattern into the flag.
     * Bit assignment: PPU worker N owns bit (EF_BIT_PPU_WORKER_BASE + N). */
    u64 my_bit = 1ULL << (EF_BIT_PPU_WORKER_BASE + ctx->idx);
    int rc = sys_event_flag_set(ctx->ef, my_bit);
    if (rc != CELL_OK) {
        fprintf(stderr, "Worker[%u]: sys_event_flag_set failed: 0x%08x\n",
                ctx->idx, (unsigned)rc);
    }

    printf("Worker[%u] is exiting.\n", ctx->idx);
    sysThreadExit(0);
}

/* Drive the SPU thread one request at a time.  Pushes the request word
 * to SNR1 first; the SPU reads SNR1, decodes, then (for set-bit ops)
 * reads SNR2 to pick up the event-flag-id.
 *
 * sysSpuThreadWriteSignal's `signal` arg is 0-based: 0 -> SNR1, 1 -> SNR2. */
static int spu_send_request(sys_spu_thread_t thr,
                            u32 opcode, u32 bit, sys_event_flag_t ef)
{
    int rc = sysSpuThreadWriteSignal(thr, /*SNR1=*/0, SPU_REQ_BUILD(opcode, bit));
    if (rc != CELL_OK) return rc;

    /* QUIT doesn't consume SNR2 on the SPU side, so skip the second
     * write — sending it would leave a stale value in the channel for
     * any future request and only confuse later debugging. */
    if (opcode == SPU_REQ_OP_QUIT) return CELL_OK;

    return sysSpuThreadWriteSignal(thr, /*SNR2=*/1, (u32)ef);
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    int rc;

    printf("Master is starting.\n");

    /* ----- create the event flag ----------------------------------- */
    sys_event_flag_t ef;
    sys_event_flag_attribute_t ef_attr;
    sys_event_flag_attribute_initialize(ef_attr);
    sys_event_flag_attribute_name_set(ef_attr.name, "hello_ef");

    rc = sys_event_flag_create(&ef, &ef_attr, /*init=*/0);
    if (rc != CELL_OK) {
        fprintf(stderr, "sys_event_flag_create failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    /* ----- spawn the PPU workers ----------------------------------- */
    sys_ppu_thread_t   ppu_tids[NUM_PPU_WORKERS];
    ppu_worker_ctx_t   ppu_ctxs[NUM_PPU_WORKERS];
    char               name_buf[16];

    for (unsigned i = 0; i < NUM_PPU_WORKERS; ++i) {
        ppu_ctxs[i].ef  = ef;
        ppu_ctxs[i].idx = i;
        snprintf(name_buf, sizeof(name_buf), "ppu_worker_%u", i);

        rc = sysThreadCreate(&ppu_tids[i],
                             ppu_worker_main, &ppu_ctxs[i],
                             WORKER_PRIORITY, WORKER_STACK,
                             /*flags=*/0, name_buf);
        if (rc != CELL_OK) {
            fprintf(stderr, "sysThreadCreate[%u] failed: 0x%08x\n",
                    i, (unsigned)rc);
            return 1;
        }
    }

    /* ----- bring up the SPU side ----------------------------------- */
    rc = sysSpuInitialize(/*max_usable_spu=*/6, /*max_raw_spu=*/0);
    if (rc != CELL_OK && rc != (int)0x80010030 /* already initialised */) {
        fprintf(stderr, "sysSpuInitialize failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    sysSpuImage spu_img;
    rc = sysSpuImageImport(&spu_img, spu_ef_bin, /*type=*/0);
    if (rc != CELL_OK) {
        fprintf(stderr, "sysSpuImageImport failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    sys_spu_group_t   spu_group;
    sysSpuThreadGroupAttribute group_attr = {
        .nsize = (u32)strlen(SPU_GROUP_NAME) + 1,
        .name  = (const char *)ptr2ea(SPU_GROUP_NAME),
        .type  = 0,
    };
    rc = sysSpuThreadGroupCreate(&spu_group, /*num=*/1,
                                 SPU_GROUP_PRIORITY, &group_attr);
    if (rc != CELL_OK) {
        fprintf(stderr, "sysSpuThreadGroupCreate failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    sysSpuThreadAttribute thr_attr = {
        .name   = (const char *)ptr2ea(SPU_THREAD_NAME),
        .nsize  = (u32)strlen(SPU_THREAD_NAME) + 1,
        .option = SPU_THREAD_ATTR_NONE,
    };
    sysSpuThreadArgument spu_args = { 0, 0, 0, 0 };

    sys_spu_thread_t  spu_thr;
    rc = sysSpuThreadInitialize(&spu_thr, spu_group, /*spu_num=*/0,
                                &spu_img, &thr_attr, &spu_args);
    if (rc != CELL_OK) {
        fprintf(stderr, "sysSpuThreadInitialize failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    /* Put both SNRs in overwrite mode so each request is delivered
     * atomically; the default (OR) would silently merge a back-to-back
     * sequence of writes if the SPU stalls between them. */
    rc = sysSpuThreadSetConfiguration(spu_thr,
                                      SPU_SIGNAL1_OVERWRITE | SPU_SIGNAL2_OVERWRITE);
    if (rc != CELL_OK) {
        fprintf(stderr, "sysSpuThreadSetConfiguration failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    rc = sysSpuThreadGroupStart(spu_group);
    if (rc != CELL_OK) {
        fprintf(stderr, "sysSpuThreadGroupStart failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    /* Tell the SPU worker to set the bit it owns. */
    rc = spu_send_request(spu_thr, SPU_REQ_OP_SET_BIT_IMP, EF_BIT_SPU_WORKER, ef);
    if (rc != CELL_OK) {
        fprintf(stderr, "spu_send_request(SET_BIT) failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    /* ----- wait for everyone --------------------------------------- */
    u64 want = 0;
    for (unsigned i = 0; i < NUM_PPU_WORKERS; ++i) {
        want |= 1ULL << (EF_BIT_PPU_WORKER_BASE + i);
    }
    want |= 1ULL << EF_BIT_SPU_WORKER;

    u64 got = 0;
    rc = sys_event_flag_wait(ef, want,
                             SYS_EVENT_FLAG_WAIT_AND | SYS_EVENT_FLAG_WAIT_CLEAR,
                             &got, /*timeout=*/0);
    if (rc != CELL_OK) {
        fprintf(stderr, "sys_event_flag_wait failed: 0x%08x\n", (unsigned)rc);
        return 1;
    }

    printf("SPU Worker finished my job (bit=%u, ef=0x%08x, tag=done)\n",
           (unsigned)EF_BIT_SPU_WORKER, (unsigned)ef);

    /* ----- shutdown ------------------------------------------------ */
    /* Tell the SPU worker to exit cleanly, then wait for the group. */
    rc = spu_send_request(spu_thr, SPU_REQ_OP_QUIT, /*bit=*/0, /*ef=*/0);
    if (rc != CELL_OK) {
        fprintf(stderr, "spu_send_request(QUIT) failed: 0x%08x\n", (unsigned)rc);
    }

    u32 join_cause = 0, join_status = 0;
    rc = sysSpuThreadGroupJoin(spu_group, &join_cause, &join_status);
    if (rc != CELL_OK) {
        fprintf(stderr, "sysSpuThreadGroupJoin failed: 0x%08x\n", (unsigned)rc);
    }

    sysSpuImageClose(&spu_img);

    /* Reap the PPU workers (they may already be done). */
    for (unsigned i = 0; i < NUM_PPU_WORKERS; ++i) {
        u64 retval = 0;
        sysThreadJoin(ppu_tids[i], &retval);
    }

    rc = sys_event_flag_destroy(ef);
    if (rc != CELL_OK) {
        fprintf(stderr, "sys_event_flag_destroy failed: 0x%08x\n", (unsigned)rc);
    }

    printf("Master is exiting.\n");
    return 0;
}
