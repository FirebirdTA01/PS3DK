/* spurs-suite SPU vector literals (row spu-vector-literals): runs the SPU
 * program that checks (vector-type)(e1, ..., en) literals and
 * (vector-type)(scalar) splats lane by lane, and reports its failure mask. */
#include "harness.h"
#include <sys/spu_initialize.h>
#include <sys/spu_image.h>
#include <sys/spu_thread.h>
#include <sys/spu_thread_group.h>
#include "../vector_literals.h"
#include SUITE_SPU_HEADER

#define VL_EBUSY 0x8001000Au

alignas(128) static volatile uint32_t s_box[4];

static int row_main()
{
    suite::watchdog(30);
    int rc = sys_spu_initialize(6, 0);
    if (rc && static_cast<unsigned>(rc) != VL_EBUSY) return suite::invalid("spu initialize", rc);
    sys_spu_image_t image;
    if ((rc = sys_spu_image_import(&image, SUITE_SPU_BIN, SYS_SPU_IMAGE_PROTECT)))
        return suite::invalid("image import", rc);
    sys_spu_thread_group_attribute_t gattr;
    sys_spu_thread_group_attribute_initialize(gattr);
    sys_spu_thread_group_attribute_name(gattr, "SuiteVl");
    sys_spu_thread_group_t group;
    if ((rc = sys_spu_thread_group_create(&group, 1, 100, &gattr))) return suite::invalid("group create", rc);
    sys_spu_thread_attribute_t tattr;
    sys_spu_thread_attribute_initialize(tattr);
    sys_spu_thread_attribute_name(tattr, "SuiteVlThr");
    sys_spu_thread_argument_t arg;
    sys_spu_thread_argument_initialize(arg);
    arg.arg1 = reinterpret_cast<uintptr_t>(&s_box[0]);
    arg.arg2 = VL_X;
    sys_spu_thread_t thread;
    if ((rc = sys_spu_thread_initialize(&thread, group, 0, &image, &tattr, &arg)))
        return suite::invalid("thread initialize", rc);

    suite::activity("running the SPU vector literal checks");
    std::memset(const_cast<uint32_t *>(&s_box[0]), 0, sizeof s_box);
    if ((rc = sys_spu_thread_group_start(group))) return suite::invalid("group start", rc);
    int cause = -1, status = -1, exitCode = -1;
    if ((rc = sys_spu_thread_group_join(group, &cause, &status))) return suite::fail("join rc", rc, 0);
    if ((rc = sys_spu_thread_get_exit_status(thread, &exitCode))) return suite::fail("exit status rc", rc, 0);
    sys_spu_thread_group_destroy(group);
    sys_spu_image_close(&image);

    if (s_box[0] != VL_MAGIC) return suite::fail("SPU reported", s_box[0], VL_MAGIC);
    if (s_box[2] != VL_X) return suite::fail("SPU saw x", s_box[2], VL_X);
    if (s_box[1] != 0) return suite::fail("failed checks (bit mask)", s_box[1], 0);
    if (exitCode != 0) return suite::fail("SPU exit code", exitCode, 0);
    return suite::ok();
}

SUITE_ENTRY_POINT(row_main)
