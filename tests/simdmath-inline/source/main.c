// tests/simdmath-inline/source/main.c
//
// PPU side of the simdmath-inline test.
//
// Spawns the embedded SPU ELF that calls the libsimdmath f4 + d2
// comparison family.  The SPU image's link line contains no -lsimdmath
// (see CMakeLists - LIBS = sputhread only), so if the inline headers
// are not actually installed the link fails before we ever reach
// main().
//
// At runtime the PPU:
//   1. initialises 6 usable SPU thread group resources,
//   2. imports the embedded SPU ELF,
//   3. creates a thread group + one thread, passing the EA of the
//      PPU's 16-byte `spu_done` slot as the thread argument,
//   4. starts the group,
//   5. busy-waits (bounded) until the SPU DMAs the `done` slot,
//   6. joins the group, reads the SPU's exit status (the SPU's
//      sys_spu_thread_exit code), and returns it as the PPU exit
//      code so the harness can assert on it.
//
// Uses the reference-SDK snake_case forwarders (sys_spu_*) over
// PSL1GHT's syscalls, exercised end-to-end here.

#include <stdio.h>
#include <stdint.h>
#include <ppu-types.h>
#include <sys/spu.h>
/* Reference-SDK snake_case SPU thread forwarders.  The base <sys/spu.h>
 * header carries PSL1GHT's sysSpu* camelCase spellings; the reference-
 * SDK snake_case wrappers (`sys_spu_*`) and the reference-SDK canonical
 * types (`sys_spu_thread_attribute_t`, `sys_spu_thread_group_attribute_t`,
 * `sys_spu_thread_argument_t`, `sys_spu_image_t`) live in these
 * companion headers, each of which includes <sys/spu.h> itself. */
#include <sys/spu_initialize.h>
#include <sys/spu_image.h>
#include <sys/spu_thread_group.h>
#include <sys/spu_thread.h>

#include "simdmath_inline_spu_bin.h"

#define ptr2ea(x) ((u64) ((uintptr_t) (x)))

/* The SPU DMAs all four 32-bit words of this slot back so the PPU's
 * bounded busy-wait has a well-defined end condition.  16-byte size
 * matches the SPU's single mfc_put (buf[4], 16 bytes).  128-byte
 * alignment matches Cell SPU DMA alignment requirements. */
static volatile u32 spu_done[4] __attribute__((aligned(128)));

int main (int argc, char **argv)
{
    (void)argc; (void)argv;
    int cause = -1, status = -1;
    int spu_exit = -1;
    int r;
    sys_spu_image_t image;
    sys_spu_thread_t thread_id;
    sys_spu_thread_group_t group_id;

    printf ("simdmath-inline(ppu): starting\n");

    r = sys_spu_initialize (6, 0);
    if (r != 0) { printf ("sys_spu_initialize failed: %d\n", r);    return 3; }

    r = sys_spu_image_import (&image, simdmath_inline_spu_bin, 0);
    if (r != 0) { printf ("sys_spu_image_import failed: %d\n", r);  return 4; }

    sys_spu_thread_group_attribute_t grpattr = {
        .nsize     = sizeof "simd_inline_grp",
        .name      = (const char *) ptr2ea ("simd_inline_grp"),
        .type      = 0,
        .option.ct = 0,
    };
    r = sys_spu_thread_group_create (&group_id, 1, 100, &grpattr);
    if (r != 0) { printf ("sys_spu_thread_group_create failed: %d\n", r); return 5; }

    sys_spu_thread_attribute_t attr = {
        .name   = (const char *) ptr2ea ("simd_inline_thr"),
        .nsize  = sizeof "simd_inline_thr",
        .option = SPU_THREAD_ATTR_NONE,
    };
    /* Reference-SDK canonical argument type (arg1..arg4); the layout is
     * identical to PSL1GHT's arg0..arg3, so the SPU's main() sees the
     * EA of `spu_done` in its first parameter. */
    sys_spu_thread_argument_t arg = {
        .arg1 = ptr2ea (spu_done),
        .arg2 = 0,
        .arg3 = 0,
        .arg4 = 0,
    };

    r = sys_spu_thread_initialize (&thread_id, group_id, 0, &image, &attr, &arg);
    if (r != 0) { printf ("sys_spu_thread_initialize failed: %d\n", r); return 6; }

    printf ("simdmath-inline(ppu): starting thread group %u\n",
            (unsigned) group_id);
    r = sys_spu_thread_group_start (group_id);
    if (r != 0) { printf ("sys_spu_thread_group_start failed: %d\n", r); return 7; }

    /* Bounded busy-wait on the first word of the 16-byte slot the SPU
     * is about to DMA back.  `spu_done` is volatile, so every iteration
     * performs a real memory load and can observe the DMA completion.
     * The iteration cap bounds the wait so a hung SPU produces a visible
     * non-zero exit code rather than an endless loop. */
    {
        const long long iter_max = 2000000000ll;  /* ~10-30 s on Cell PPU0 */
        long long i;
        for (i = 0; i < iter_max && spu_done [0] == 0; i++) {
            __asm__ volatile ("" : : : "memory");   /* compiler barrier */
        }
        if (spu_done [0] == 0) {
            printf ("simdmath-inline(ppu): timeout waiting for SPU "
                    "completion DMA; giving up (no SPU exit to report)\n");
            sys_spu_image_close (&image);
            return 8;
        }
    }

    r = sys_spu_thread_group_join (group_id, &cause, &status);
    if (r != 0) { printf ("sys_spu_thread_group_join failed: %d\n", r); }

    /* The join status word is the group's, not the SPU's.  The SPU's
     * sys_spu_thread_exit(fail) code lands in the SPU thread's own
     * exit status word - read it separately. */
    r = sys_spu_thread_get_exit_status (thread_id, &spu_exit);
    if (r != 0) { printf ("sys_spu_thread_get_exit_status failed: %d\n", r); }

    sys_spu_image_close (&image);

    printf ("simdmath-inline: spu finished cause=%d status=%d "
            "done=0x%08x exit=%d\n",
            cause, status, (unsigned) spu_done [0], spu_exit);

    /* The SPU exits with 0 if every f4 + d2 check passed.  Propagate
     * that as our own exit code so the harness can assert 0 success /
     * nonzero failure directly. */
    return spu_exit;
}
