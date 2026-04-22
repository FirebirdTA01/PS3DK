/*
 * compact-opd-probe — proof-of-concept for 8-byte compact function
 * descriptors.
 *
 * compact_target.S defines a function `compact_target` whose function
 * descriptor is hand-emitted as 8 bytes instead of the standard
 * ELFv1 24-byte form.
 *
 * Here we call through it using the reference SDK's 2-read indirect-
 * call sequence (lwz r0,0(r11); lwz r2,4(r11); mtctr; bctrl), written
 * as inline asm so no compiler patch is required for this test.
 *
 * Expected behaviour:
 *   - compact_target(42) returns 142 → "PASS"
 *   - any crash or wrong return value → spec revisit needed
 *
 * Sanity-checks before we commit to the full compiler+linker patches
 * the compact-opd migration requires.
 */

#include <stdio.h>
#include <stdint.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* compact_target's symbol resolves to the address of its 8-byte
 * function descriptor — same as any function pointer, just that the
 * descriptor there is 8 bytes long instead of 24. */
extern int compact_target(int);

static int call_via_compact_descriptor(int (*fp)(int), int arg)
{
    int result;
    uint32_t saved_r2;

    /* Save caller TOC on stack (standard PPC64 cross-TOC ABI). */
    __asm__ __volatile__ (
        "std     2, %0\n\t"
        : "=m"(saved_r2)
    );

    /* 2-read indirect call. r11 = fp (descriptor EA).
     *   lwz r0, 0(r11)   ; r0 = entry EA  (low 32 bits of fp)
     *   lwz r2, 4(r11)   ; r2 = callee TOC
     *   mtctr r0
     *   bctrl
     * Pass `arg` in r3, get result in r3. */
    __asm__ __volatile__ (
        "mr      11, %1\n\t"
        "mr      3, %2\n\t"
        "lwz     0, 0(11)\n\t"
        "mtctr   0\n\t"
        "lwz     2, 4(11)\n\t"
        "bctrl\n\t"
        "mr      %0, 3\n\t"
        : "=r"(result)
        : "r"(fp), "r"(arg)
        : "r0", "r3", "r11", "ctr", "lr", "memory"
    );

    /* Restore TOC. */
    __asm__ __volatile__ (
        "ld      2, %0\n\t"
        :
        : "m"(saved_r2)
    );

    return result;
}

int main(void)
{
    printf("compact-opd-probe: begin\n");

    int (*fp)(int) = compact_target;
    printf("  descriptor at %p\n", (void *)fp);

    int r = call_via_compact_descriptor(fp, 42);
    printf("  compact_target(42) returned %d (expected 142)\n", r);

    if (r == 142) {
        printf("compact-opd-probe: result: PASS\n");
    } else {
        printf("compact-opd-probe: result: FAIL (got %d, expected 142)\n", r);
    }
    printf("compact-opd-probe: done\n");
    return 0;
}
