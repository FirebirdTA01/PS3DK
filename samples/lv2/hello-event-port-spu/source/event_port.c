/* hello-event-port-spu - exercise the SPU side of lv2 event ports.
 *
 * Coverage:
 *   sys_spu_thread_send_event       (SPU -> PPU queue)
 *   sys_spu_thread_throw_event      (SPU -> PPU queue, fire-and-forget)
 *   sys_spu_thread_receive_event    (PPU port -> SPU spuq, blocking)
 *   sys_spu_thread_tryreceive_event (PPU port -> SPU spuq, non-blocking)
 *   sys_spu_thread_group_yield      (cooperative yield trap)
 *
 * Bring-up:
 *   1. Create two event queues:
 *        - send_q  receives events the SPU sends back to us
 *        - recv_q  carries events the PPU sends to the SPU
 *   2. Create a port + connect it locally to recv_q so we can drive
 *      SPU events from the PPU side via sys_event_port_send.
 *   3. Initialise an SPU thread group + thread with our spu image.
 *   4. Wire the SPU-side ports:
 *        sysSpuThreadConnectEvent(thread, send_q, USER, SPU_PORT_SEND)
 *        sysSpuThreadBindQueue   (thread, recv_q, SPU_QUEUE_RECV)
 *   5. Start the group; the SPU runs through a fixed test sequence:
 *        a. send_event(spup=SPU_PORT_SEND, data0=0xAAAA01, data1=0x1111)
 *        b. throw_event(spup=SPU_PORT_SEND, data0=0xAAAA02, data1=0x2222)
 *        c. tryreceive_event(spuq=SPU_QUEUE_RECV) -> expects EAGAIN
 *           (no event yet)
 *        d. group_yield (no-op for a single-thread group, but the
 *           syscall itself must not crash)
 *        e. receive_event(spuq=SPU_QUEUE_RECV) blocks until PPU sends
 *           (data2=0xBEEF, data3=0xCAFE)
 *        f. send_event(... data0=0xAAAA03, data1=0x3333) on success
 *   6. PPU pulls each expected event off send_q and validates payload.
 *   7. Tear down + join.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-types.h>
#include <sys/event.h>
#include <sys/spu.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#include "spu_ep_bin.h"

#define MASTER_PRIORITY      1000
#define SPU_GROUP_PRIORITY   100
#define SPU_GROUP_NAME       "ep_spu_group"
#define SPU_THREAD_NAME      "ep_spu_worker"

/* Wire constants - keep PPU + SPU agreement in one place. */
#define SPU_PORT_SEND        1   /* spup the SPU sends events out on */
#define SPU_QUEUE_RECV       2   /* spuq the SPU pulls events from   */

#define SEND_EVENT_TYPE      1   /* SYS_SPU_THREAD_EVENT_USER */
#define USER_EVENT_TYPE_KEY  0xc0ffeeULL

#define ptr2ea(p)            ((u64)(uintptr_t)(p))

static int recv_one(sys_event_queue_t q, uint32_t want_d0_lo, uint32_t want_d1)
{
    sys_event_t ev;
    int rc = sys_event_queue_receive(q, &ev, /*timeout=*/0);
    if (rc != CELL_OK) {
        fprintf(stderr, "  sys_event_queue_receive: 0x%08x\n", (unsigned)rc);
        return rc;
    }
    /* sys_event_t.data_2/data_3 hold the SPU-supplied (data0_lo, data1).
     * data_1 is the source port id<<32 | misc; we only sanity-check the
     * SPU-controlled words. */
    uint32_t got_d0_lo = (uint32_t)ev.data_2;
    uint32_t got_d1    = (uint32_t)ev.data_3;
    printf("  PPU rx: data0_lo=%#x data1=%#x  (want %#x / %#x)\n",
           (unsigned)got_d0_lo, (unsigned)got_d1,
           (unsigned)want_d0_lo, (unsigned)want_d1);
    return (got_d0_lo == want_d0_lo && got_d1 == want_d1) ? CELL_OK : -1;
}

int main(void)
{
    int rc;

    printf("hello-event-port-spu: SPU event-port API end-to-end\n");

    /* ---- queue / port setup ------------------------------------------ */
    sys_event_queue_attr_t qattr;
    sys_event_queue_attribute_initialize(qattr);

    sys_event_queue_t send_q = 0, recv_q = 0;
    rc = sys_event_queue_create(&send_q, &qattr, /*key=*/0, /*size=*/16);
    if (rc) { fprintf(stderr, "create send_q: 0x%08x\n", (unsigned)rc); return 1; }
    /* recv_q must be SYS_EVENT_QUEUE_SPU because sys_spu_thread_bind_queue
     * insists on a SPU-typed queue (CELL_EINVAL otherwise). */
    qattr.type = SYS_EVENT_QUEUE_SPU;
    rc = sys_event_queue_create(&recv_q, &qattr, /*key=*/0, /*size=*/16);
    if (rc) { fprintf(stderr, "create recv_q: 0x%08x\n", (unsigned)rc); return 1; }

    sys_event_port_t recv_port = 0;
    rc = sys_event_port_create(&recv_port, SYS_EVENT_PORT_LOCAL, USER_EVENT_TYPE_KEY);
    if (rc) { fprintf(stderr, "port_create: 0x%08x\n", (unsigned)rc); return 1; }
    rc = sys_event_port_connect_local(recv_port, recv_q);
    if (rc) { fprintf(stderr, "port_connect: 0x%08x\n", (unsigned)rc); return 1; }
    printf("  queues + port up\n");

    /* ---- SPU bring-up ------------------------------------------------ */
    rc = sysSpuInitialize(/*max_usable_spu=*/6, /*max_raw_spu=*/0);
    if (rc != CELL_OK && rc != (int)0x80010030) {
        fprintf(stderr, "sysSpuInitialize: 0x%08x\n", (unsigned)rc); return 1;
    }

    sysSpuImage spu_img;
    rc = sysSpuImageImport(&spu_img, spu_ep_bin, /*type=*/0);
    if (rc) { fprintf(stderr, "sysSpuImageImport: 0x%08x\n", (unsigned)rc); return 1; }

    sysSpuThreadGroupAttribute gattr = {
        .nsize = (u32)strlen(SPU_GROUP_NAME) + 1,
        .name  = (const char *)ptr2ea(SPU_GROUP_NAME),
        .type  = 0,
    };
    sys_spu_group_t  spu_group;
    rc = sysSpuThreadGroupCreate(&spu_group, /*num=*/1, SPU_GROUP_PRIORITY, &gattr);
    if (rc) { fprintf(stderr, "ThreadGroupCreate: 0x%08x\n", (unsigned)rc); return 1; }

    sysSpuThreadAttribute tattr = {
        .name   = (const char *)ptr2ea(SPU_THREAD_NAME),
        .nsize  = (u32)strlen(SPU_THREAD_NAME) + 1,
        .option = SPU_THREAD_ATTR_NONE,
    };
    sysSpuThreadArgument args = { 0, 0, 0, 0 };

    sys_spu_thread_t  spu_thr;
    rc = sysSpuThreadInitialize(&spu_thr, spu_group, /*spu_num=*/0,
                                &spu_img, &tattr, &args);
    if (rc) { fprintf(stderr, "ThreadInitialize: 0x%08x\n", (unsigned)rc); return 1; }

    /* Wire the ports.  Once the group starts, the SPU side can call
     * sys_spu_thread_send_event / receive_event against these. */
    rc = sysSpuThreadConnectEvent(spu_thr, send_q, SEND_EVENT_TYPE, SPU_PORT_SEND);
    if (rc) { fprintf(stderr, "ConnectEvent: 0x%08x\n", (unsigned)rc); return 1; }

    rc = sysSpuThreadBindQueue(spu_thr, recv_q, SPU_QUEUE_RECV);
    if (rc) { fprintf(stderr, "BindQueue: 0x%08x\n", (unsigned)rc); return 1; }
    printf("  SPU thread wired (send_port=%d, recv_queue=%d)\n",
           SPU_PORT_SEND, SPU_QUEUE_RECV);

    rc = sysSpuThreadGroupStart(spu_group);
    if (rc) { fprintf(stderr, "ThreadGroupStart: 0x%08x\n", (unsigned)rc); return 1; }

    /* ---- exchange ---------------------------------------------------- */
    printf("\n--- step a: SPU send_event ---\n");
    if (recv_one(send_q, 0xAAAA01, 0x1111) != CELL_OK) goto fail;

    printf("\n--- step b: SPU throw_event ---\n");
    if (recv_one(send_q, 0xAAAA02, 0x2222) != CELL_OK) goto fail;

    printf("\n--- step c-d: SPU tryreceive_event (empty) + group_yield ---\n");
    /* Probe: peek at the actual rc the SPU got from tryreceive.  The
     * SPU sends it back as data1.  Followed by group_yield, which is
     * a kernel pass-through (no event). */
    {
        sys_event_t ev;
        int qrc = sys_event_queue_receive(send_q, &ev, /*timeout=*/0);
        if (qrc) { fprintf(stderr, "rx tryrecv probe: 0x%08x\n", (unsigned)qrc); goto fail; }
        printf("  SPU tryreceive_event returned: %#x  (expect 0x8001000a EBUSY)\n",
               (unsigned)ev.data_3);
    }

    printf("\n--- step e: PPU sends event to SPU's recv queue ---\n");
    rc = sys_event_port_send(recv_port,
                             /*data0=*/0xDEAD0001ULL,
                             /*data1=*/0xBEEFULL,
                             /*data2=*/0xCAFEULL);
    if (rc) { fprintf(stderr, "port_send: 0x%08x\n", (unsigned)rc); goto fail; }

    printf("\n--- step f: SPU acknowledges with send_event ---\n");
    if (recv_one(send_q, 0xAAAA03, 0x3333) != CELL_OK) goto fail;

    /* ---- shutdown --------------------------------------------------- */
    u32 join_cause = 0, join_status = 0;
    rc = sysSpuThreadGroupJoin(spu_group, &join_cause, &join_status);
    if (rc) fprintf(stderr, "ThreadGroupJoin: 0x%08x\n", (unsigned)rc);
    printf("\n  SPU group joined (cause=%u status=%u)\n",
           (unsigned)join_cause, (unsigned)join_status);

    sysSpuImageClose(&spu_img);
    sys_event_port_disconnect(recv_port);
    sys_event_port_destroy(recv_port);
    sys_event_queue_destroy(send_q, /*mode=*/0);
    sys_event_queue_destroy(recv_q, /*mode=*/0);

    printf("\nSUCCESS\n");
    return 0;

fail:
    fprintf(stderr, "FAILURE\n");
    /* Best-effort teardown so RPCS3 closes cleanly. */
    sysSpuThreadGroupTerminate(spu_group, -1);
    u32 c = 0, s = 0;
    sysSpuThreadGroupJoin(spu_group, &c, &s);
    sys_process_exit(1);
}
