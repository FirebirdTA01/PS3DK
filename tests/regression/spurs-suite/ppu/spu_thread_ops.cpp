/* spurs-suite SPU thread operations (row spu-thread-ops): the Lv-2 SPU
 * thread and group calls the SPURS samples use, on a plain SPU thread.
 *   cfg       sys_spu_thread_set_spu_cfg / get_spu_cfg round trip
 *   priority  sys_spu_thread_group_set_priority / get_priority round trip
 *   event     sys_spu_thread_connect_event: the SPU's user event reaches a
 *             PPU event queue with the documented layout (source key,
 *             thread id, port, data0, data1)
 *   ls        sys_spu_thread_write_ls before the SPU reads it, and
 *             sys_spu_thread_read_ls of what the SPU computed, also while
 *             the group is suspended
 *   mailbox   sys_spu_thread_write_spu_mb drives the SPU's two steps
 *   exit      the SPU's exit code is the second mailbox word
 *   queues    two SYS_EVENT_QUEUE_LOCAL queues coexist (the local key) */
#include "harness.h"
#include <sys/spu_initialize.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include <sys/event.h>
#include "../spu_thread_ops.h"
#include SUITE_SPU_HEADER

#define STO_EBUSY 0x8001000Au

alignas(128) static volatile uint32_t s_box[4];

static int row_main()
{
    suite::watchdog(30);
    int rc = sys_spu_initialize(6, 0);
    if (rc && static_cast<unsigned>(rc) != STO_EBUSY) return suite::invalid("spu initialize", rc);

    sys_spu_image_t image;
    if ((rc = sys_spu_image_import(&image, SUITE_SPU_BIN, SYS_SPU_IMAGE_PROTECT)))
        return suite::invalid("image import", rc);

    sys_spu_thread_group_attribute_t gattr;
    sys_spu_thread_group_attribute_initialize(gattr);
    sys_spu_thread_group_attribute_name(gattr, "SuiteSto");
    sys_spu_thread_group_t group;
    if ((rc = sys_spu_thread_group_create(&group, 1, 100, &gattr)))
        return suite::invalid("group create", rc);

    sys_spu_thread_attribute_t tattr;
    sys_spu_thread_attribute_initialize(tattr);
    sys_spu_thread_attribute_name(tattr, "SuiteStoThr");
    sys_spu_thread_argument_t arg;
    sys_spu_thread_argument_initialize(arg);
    arg.arg1 = reinterpret_cast<uintptr_t>(&s_box[0]);
    sys_spu_thread_t thread;
    if ((rc = sys_spu_thread_initialize(&thread, group, 0, &image, &tattr, &arg)))
        return suite::invalid("thread initialize", rc);

    suite::activity("SPU configuration round trip");
    uint64_t cfg = 0;
    if ((rc = sys_spu_thread_set_spu_cfg(thread, 1))) return suite::fail("set spu cfg rc", rc, 0);
    if ((rc = sys_spu_thread_get_spu_cfg(thread, &cfg))) return suite::fail("get spu cfg rc", rc, 0);
    if (cfg != 1) return suite::fail("spu cfg", static_cast<unsigned>(cfg), 1);

    suite::activity("group priority round trip");
    int prio = 0;
    if ((rc = sys_spu_thread_group_set_priority(group, 150))) return suite::fail("set priority rc", rc, 0);
    if ((rc = sys_spu_thread_group_get_priority(group, &prio))) return suite::fail("get priority rc", rc, 0);
    if (prio != 150) return suite::fail("group priority", prio, 150);

    suite::activity("connecting the SPU's user events to a queue");
    sys_event_queue_attribute_t qattr;
    sys_event_queue_attribute_initialize(qattr);
    sys_event_queue_t queue;
    if ((rc = sys_event_queue_create(&queue, &qattr, SYS_EVENT_QUEUE_LOCAL, 4)))
        return suite::invalid("event queue create", rc);
    /* SYS_EVENT_QUEUE_LOCAL is the private key: a second local queue must
       not collide with the first (it did while the name meant key 1) */
    sys_event_queue_t second;
    if ((rc = sys_event_queue_create(&second, &qattr, SYS_EVENT_QUEUE_LOCAL, 4)))
        return suite::fail("second local event queue rc", rc, 0);
    sys_event_queue_destroy(second, 0);
    if ((rc = sys_spu_thread_connect_event(thread, queue, SYS_SPU_THREAD_EVENT_USER, STO_PORT)))
        return suite::fail("connect event rc", rc, 0);

    suite::activity("starting the SPU thread");
    std::memset(const_cast<uint32_t *>(&s_box[0]), 0, sizeof s_box);
    if ((rc = sys_spu_thread_group_start(group))) return suite::invalid("group start", rc);
    if (!suite::wait_for([] { return s_box[0] == STO_MAGIC; }))
        return suite::fail("SPU published its LS addresses", s_box[0], STO_MAGIC);
    const uint32_t lsIn = s_box[1], lsOut = s_box[2];

    suite::activity("write_ls %#x, then mailbox %#x", lsIn, STO_M1);
    if ((rc = sys_spu_thread_write_ls(thread, lsIn, STO_IN, 4))) return suite::fail("write ls rc", rc, 0);
    if ((rc = sys_spu_thread_write_spu_mb(thread, STO_M1))) return suite::fail("write mailbox 1 rc", rc, 0);

    suite::activity("waiting for the SPU's user event");
    sys_event_t ev;
    std::memset(&ev, 0, sizeof ev);
    if ((rc = sys_event_queue_receive(queue, &ev, 5 * 1000 * 1000))) return suite::fail("receive event rc", rc, 0);
    const uint32_t want = STO_IN + STO_M1;
    if (ev.source != SYS_SPU_THREAD_EVENT_USER_KEY)
        return suite::fail("event source (low word)", static_cast<unsigned>(ev.source), static_cast<unsigned>(SYS_SPU_THREAD_EVENT_USER_KEY));
    if (ev.data1 != thread) return suite::fail("event thread id", static_cast<unsigned>(ev.data1), thread);
    if (((ev.data2 >> 32) & 0xff) != STO_PORT)
        return suite::fail("event port", static_cast<unsigned>(ev.data2 >> 32), STO_PORT);
    if ((ev.data2 & 0xffffff) != (want & 0xffffff))
        return suite::fail("event data0", static_cast<unsigned>(ev.data2), want & 0xffffff);
    if (ev.data3 != STO_M1) return suite::fail("event data1", static_cast<unsigned>(ev.data3), STO_M1);

    suite::activity("read_ls %#x", lsOut);
    uint64_t v = 0;
    if ((rc = sys_spu_thread_read_ls(thread, lsOut, &v, 4))) return suite::fail("read ls rc", rc, 0);
    if (v != want) return suite::fail("read ls value", static_cast<unsigned>(v), want);

    suite::activity("suspend, read_ls while suspended, resume");
    if ((rc = sys_spu_thread_group_suspend(group))) return suite::fail("suspend rc", rc, 0);
    v = 0;
    if ((rc = sys_spu_thread_read_ls(thread, lsIn, &v, 4))) return suite::fail("read ls suspended rc", rc, 0);
    if (v != STO_IN) return suite::fail("read ls suspended value", static_cast<unsigned>(v), STO_IN);
    if ((rc = sys_spu_thread_group_resume(group))) return suite::fail("resume rc", rc, 0);

    suite::activity("mailbox %#x lets the SPU exit", STO_M2);
    if ((rc = sys_spu_thread_write_spu_mb(thread, STO_M2))) return suite::fail("write mailbox 2 rc", rc, 0);
    int cause = -1, status = -1, exitCode = -1;
    if ((rc = sys_spu_thread_group_join(group, &cause, &status))) return suite::fail("join rc", rc, 0);
    if ((rc = sys_spu_thread_get_exit_status(thread, &exitCode))) return suite::fail("exit status rc", rc, 0);
    if (exitCode != static_cast<int>(STO_M2)) return suite::fail("SPU exit code", exitCode, STO_M2);

    if ((rc = sys_spu_thread_disconnect_event(thread, SYS_SPU_THREAD_EVENT_USER, STO_PORT)))
        return suite::fail("disconnect event rc", rc, 0);
    sys_event_queue_destroy(queue, 0);
    sys_spu_thread_group_destroy(group);
    sys_spu_image_close(&image);
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
