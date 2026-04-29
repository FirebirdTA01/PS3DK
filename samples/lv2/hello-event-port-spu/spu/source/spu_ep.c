/* hello-event-port-spu - SPU side.
 *
 * Drives a fixed test sequence against the libsputhread event-port
 * syscalls.  PPU side validates each observable result by reading
 * events off the queue we send to.
 *
 * Coverage in order:
 *   sys_spu_thread_send_event       (a, f)
 *   sys_spu_thread_throw_event      (b)
 *   sys_spu_thread_tryreceive_event (c -- expect 0x8001000a EAGAIN)
 *   sys_spu_thread_group_yield      (d -- pass-through, must not crash)
 *   sys_spu_thread_receive_event    (e -- block until PPU sends)
 */

#include <stdint.h>
#include <spu_intrinsics.h>
#include <sys/spu_thread.h>
#include <sys/spu_event.h>

#define SPU_PORT_SEND       1
#define SPU_QUEUE_RECV      2
#define EAGAIN_LV2          0x8001000a

int main(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    (void)a1; (void)a2; (void)a3; (void)a4;

    int rc;

    /* (a) basic send: data0 low 24 bits + data1 mailbox */
    rc = sys_spu_thread_send_event(SPU_PORT_SEND, 0xAAAA01, 0x1111);
    if (rc != 0) sys_spu_thread_group_exit(0xa01);

    /* (b) throw: same wire encoding plus 0x40000000 marker, no reply */
    rc = sys_spu_thread_throw_event(SPU_PORT_SEND, 0xAAAA02, 0x2222);
    if (rc != 0) sys_spu_thread_group_exit(0xb02);

    /* (c) tryreceive on empty queue should report EBUSY (0x8001000a). */
    uint32_t d1 = 0, d2 = 0, d3 = 0;
    rc = sys_spu_thread_tryreceive_event(SPU_QUEUE_RECV, &d1, &d2, &d3);
    /* Probe: send the rc back to the PPU before deciding so we can
     * see exactly what the kernel returned. */
    sys_spu_thread_send_event(SPU_PORT_SEND, 0xCC0001, (uint32_t)rc);
    if (rc != (int)EAGAIN_LV2) sys_spu_thread_group_exit(0xc03);

    /* (d) cooperative yield. */
    sys_spu_thread_group_yield();

    /* (e) blocking receive. */
    rc = sys_spu_thread_receive_event(SPU_QUEUE_RECV, &d1, &d2, &d3);
    if (rc != 0) sys_spu_thread_group_exit(0xe05);
    if (d2 != 0xBEEF || d3 != 0xCAFE) sys_spu_thread_group_exit(0xe06);

    /* (f) acknowledge by sending a final event back. */
    rc = sys_spu_thread_send_event(SPU_PORT_SEND, 0xAAAA03, 0x3333);
    if (rc != 0) sys_spu_thread_group_exit(0xf07);

    sys_spu_thread_exit(0);
}
