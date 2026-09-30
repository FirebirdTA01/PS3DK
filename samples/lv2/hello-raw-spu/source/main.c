/* hello-raw-spu: create a raw SPU, load an SPU program into it, and run it
 * by hand through the problem-state registers.
 *
 *   1. sys_raw_spu_create, sys_spu_image_import + sys_raw_spu_image_load
 *   2. read the program's first instruction back from the local store
 *      (sys_raw_spu_mmio_read_ls, LS_BASE_ADDR)
 *   3. SPU_In_MBox <- 41, SPU_NPC <- entry point, SPU_RunCntl <- run
 *   4. wait for SPU_MBox_Status, read SPU_Out_MBox (expect 3 * 41 + 1)
 *   5. wait for SPU_Status to leave the running state, check the stop code
 *
 * Prints HELLO_RAW_SPU OK, or HELLO_RAW_SPU FAIL <step>. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/process.h>
#include <sys/raw_spu.h>
#include <sys/spu_image.h>
#include <sys/spu_initialize.h>
#include <sys/timer.h>

#include "spu_raw_bin.h"

SYS_PROCESS_PARAM(1001, 0x10000);

#define SPU_STATUS_RUNNING  0x1u
#define SPU_RUN             0x1u

static int fail(const char *step, uint32_t got, uint32_t want)
{
    printf("HELLO_RAW_SPU FAIL %s got=0x%x want=0x%x\n", step, (unsigned)got, (unsigned)want);
    fflush(stdout);
    return 1;
}

/* poll a condition for up to about two seconds */
#define WAIT_FOR(cond) ({ int ok_ = 0; for (int i_ = 0; i_ < 20000; ++i_) { if (cond) { ok_ = 1; break; } sys_timer_usleep(100); } ok_; })

int main(void)
{
    sys_raw_spu_t id;
    sys_spu_image_t img;
    int rc;

    if ((rc = sys_spu_initialize(6, 1)) != 0) return fail("sys_spu_initialize", rc, 0);
    if ((rc = sys_raw_spu_create(&id, NULL)) != 0) return fail("sys_raw_spu_create", rc, 0);
    printf("raw SPU %u: local store at 0x%08lx, problem state at 0x%08lx\n",
           (unsigned)id, (unsigned long)LS_BASE_ADDR(id), (unsigned long)PROB_BASE_ADDR(id));

    if ((rc = sys_spu_image_import(&img, spu_raw_bin, SYS_SPU_IMAGE_DIRECT)) != 0) return fail("image import", rc, 0);
    if ((rc = sys_raw_spu_image_load(id, &img)) != 0) return fail("image load", rc, 0);

    /* the loaded program is visible in the local store */
    uint32_t first = sys_raw_spu_mmio_read_ls(id, img.entry_point);
    printf("entry 0x%05x, first instruction 0x%08x\n", (unsigned)img.entry_point, (unsigned)first);
    if (first == 0) return fail("program in local store", first, 1);

    sys_raw_spu_mmio_write(id, SPU_In_MBox, 41);
    sys_raw_spu_mmio_write(id, SPU_NPC, img.entry_point);
    sys_raw_spu_mmio_write(id, SPU_RunCntl, SPU_RUN);

    if (!WAIT_FOR((sys_raw_spu_mmio_read(id, SPU_MBox_Status) & 0xffu) != 0))
        return fail("outbound mailbox", sys_raw_spu_mmio_read(id, SPU_MBox_Status), 1);
    uint32_t answer = sys_raw_spu_mmio_read(id, SPU_Out_MBox);
    if (answer != 3 * 41 + 1) return fail("answer", answer, 3 * 41 + 1);

    if (!WAIT_FOR((sys_raw_spu_mmio_read(id, SPU_Status) & SPU_STATUS_RUNNING) == 0))
        return fail("SPU stopped", sys_raw_spu_mmio_read(id, SPU_Status), 0);
    uint32_t status = sys_raw_spu_mmio_read(id, SPU_Status);
    if ((status >> 16) != 0x123) return fail("stop code", status >> 16, 0x123);
    printf("answer %u, SPU_Status 0x%08x (stop code 0x%x)\n", (unsigned)answer, (unsigned)status, (unsigned)(status >> 16));

    sys_spu_image_close(&img);
    if ((rc = sys_raw_spu_destroy(id)) != 0) return fail("sys_raw_spu_destroy", rc, 0);
    printf("HELLO_RAW_SPU OK\n");
    return 0;
}
