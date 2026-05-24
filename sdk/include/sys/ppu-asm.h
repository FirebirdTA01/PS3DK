/*
 * sys/ppu-asm.h — PPU low-level ABI glue (independent).
 *
 * Independent derived from docs/abi/cellos-lv2-abi-spec.md §2
 * ("Compact function-descriptor format (.opd)") and §4
 * ("Pointer and addressing model — ELF64 + ILP32 hybrid").
 * No third-party source was copied; the macros below are standard
 * PowerPC load/store/byte-reverse/timebase idioms plus the
 * descriptor-EA accessors implied directly by the spec's 8-byte
 * .opd layout:
 *
 *     offset  size  contents
 *     ------  ----  ------------------------------------------
 *     0x00    4     function entry-point EA (32-bit)
 *     0x04    4     module TOC base EA (32-bit)
 *
 * Because every userland EA fits in 32 bits (spec §4) and a C
 * function pointer's *value* already IS the 32-bit EA of its 8-byte
 * compact descriptor, the descriptor accessors are pure address
 * arithmetic — there is nothing to "step over" the way the legacy
 * 24-byte ELFv1 + sprxlinker packing required.
 *
 * This header is ABI-correct for BOTH supported pointer models:
 *   - default ELF64 + ILP32 hybrid (sizeof(void*) == 4)
 *   - opt-in -mlp64 LP64           (sizeof(void*) == 8)
 * In both cases an .opd descriptor address is 32-bit-representable,
 * so the integer accessors are exact.
 */

#ifndef _PS3DK_SYS_PPU_ASM_H_
#define _PS3DK_SYS_PPU_ASM_H_

/*
 * <ppu-types.h> MUST be the first include here.  It pulls the newlib
 * system headers (<stdint.h>, <stdlib.h>, <sys/types.h>) in the one
 * order newlib's <sys/cdefs.h> BSD-attribute gating tolerates.
 * Pulling a bare <stdint.h> ahead of it makes newlib's <stdlib.h>
 * expand __malloc_like / __result_use_check before <sys/cdefs.h>
 * defines them, which breaks the build of any TU that happens to
 * #include <stdint.h> before this header.  Do not reorder.
 * <ppu-types.h> already provides uintptr_t and u8..u64 / opd32.
 */
#include <ppu-types.h>          /* opd32 / opd64 / u8..u64 / ieee* / uintptr_t */

/*
 * The legacy top-level <ppu-asm.h> (PSL1GHT-derived, still installed
 * at $PS3DK/ppu/include/ppu-asm.h and pulled transitively by several
 * SDK headers, e.g. <sys/spu.h>) defines the SAME macro / inline
 * surface.  To let this header coexist with it in either include
 * order without -Wbuiltin-macro-redefined / redefinition warnings,
 * every definition below is guarded with #ifndef and the byte-reverse
 * inlines are gated by a one-shot feature macro.  Whichever header is
 * seen first wins; this one is a strict superset and never clobbers an
 * existing definition.  __PPU_ASM_H__ is the legacy header's own guard
 * — defining the legacy macros' names is enough; we do not redefine.
 */

#ifndef PPU_ALIGNMENT
#define PPU_ALIGNMENT           8
#endif

/*
 * __get_opd32(opd) — NULL-preserving identity that yields the 32-bit
 * effective address of a function's compact 8-byte .opd descriptor as
 * an integer.
 *
 * Spec §2: a C function pointer under the compact-OPD ABI is itself
 * the descriptor EA ([entry_ea@0, toc_ea@4]); the kernel / SPRX
 * trampoline reads those two words directly.  So this is a bare cast
 * with NULL preserved.  The unsigned-long-long return type is kept so
 * callers that stash the value in a 64-bit register before narrowing
 * still type-check under either pointer model.
 */
#ifndef __get_opd32
#define __get_opd32(opd)                                                \
    ((unsigned long long)((uintptr_t)(opd) ? (uintptr_t)(opd) : 0ULL))
#endif

/*
 * __get_addr32(addr) — low 32 bits of an address/pointer/integer as a
 * plain unsigned int.  Used to materialize a 32-bit EA slot from a
 * possibly-64-bit register value.  Spec §4: userland EAs always fit
 * in 32 bits, so the truncation is lossless for well-formed input.
 */
#ifndef __get_addr32
#define __get_addr32(addr)                                              \
    ((unsigned int)((unsigned long long)(uintptr_t)(addr)))
#endif

/*
 * __build_opd32(opd_in, opd_out) — materialize a 32-bit
 * {entry_ea, toc_ea} pair at *opd_out from a source descriptor
 * *opd_in.
 *
 * Spec §2 says the on-the-wire compact descriptor is two 32-bit words.
 * This glue tolerates a source descriptor in either shape:
 *   - native compact 8-byte form  [u32 entry, u32 toc]   (default)
 *   - legacy ELFv1 24-byte form   [u64 func,  u64 toc, …] (rare,
 *     only if a legacy object reaches the link)
 * It loads the entry/TOC fields as 64-bit registers (works for both
 * layouts on big-endian: the compact 32-bit word sits in the low half
 * of the doubleword the loader resolved) and stores them back as the
 * two 32-bit words the compact ABI mandates.  Evaluates to the 32-bit
 * EA of the freshly built descriptor.
 *
 * Implemented as a __volatile__ asm block so the compiler does not
 * reorder it around the consuming SPRX/syscall edge.
 */
#ifndef __build_opd32
#define __build_opd32(opd_in, opd_out) __extension__                    \
    ({                                                                  \
        register unsigned long long __opd_func, __opd_toc;              \
        __asm__ __volatile__(                                           \
            "ld  %0,0(%2)\n\t"                                          \
            "stw %0,0(%3)\n\t"                                          \
            "ld  %1,8(%2)\n\t"                                          \
            "stw %1,4(%3)"                                              \
            : "=&r"(__opd_func), "=&r"(__opd_toc)                       \
            : "b"((opd_in)), "b"((opd_out))                             \
            : "memory");                                                \
        (unsigned int)((unsigned long long)(uintptr_t)(opd_out));       \
    })
#endif

/*
 * Sized memory accessors.  Each load is followed by a `sync` and each
 * store by an `eieio` so the access is ordered with respect to the
 * surrounding device / cross-CPU traffic — standard PPC MMIO idiom.
 * The "b" constraint forces a base register so a bare `0(%1)`
 * displacement is legal.
 */
#ifndef __read8
#define __read8(addr) __extension__                                     \
    ({  register unsigned char  __r;                                    \
        __asm__ __volatile__("lbz %0,0(%1)\n\tsync"                      \
            : "=r"(__r) : "b"((addr)));                                  \
        __r; })
#endif

#ifndef __read16
#define __read16(addr) __extension__                                    \
    ({  register unsigned short __r;                                    \
        __asm__ __volatile__("lhz %0,0(%1)\n\tsync"                      \
            : "=r"(__r) : "b"((addr)));                                  \
        __r; })
#endif

#ifndef __read32
#define __read32(addr) __extension__                                    \
    ({  register unsigned int   __r;                                    \
        __asm__ __volatile__("lwz %0,0(%1)\n\tsync"                      \
            : "=r"(__r) : "b"((addr)));                                  \
        __r; })
#endif

#ifndef __read64
#define __read64(addr) __extension__                                    \
    ({  register unsigned long long __r;                                \
        __asm__ __volatile__("ld %0,0(%1)\n\tsync"                       \
            : "=r"(__r) : "b"((addr)));                                  \
        __r; })
#endif

#ifndef __write8
#define __write8(addr, val)                                             \
    __asm__ __volatile__("stb %0,0(%1)\n\teieio"                        \
        : : "r"((val)), "b"((addr)) : "memory")
#endif

#ifndef __write16
#define __write16(addr, val)                                            \
    __asm__ __volatile__("sth %0,0(%1)\n\teieio"                        \
        : : "r"((val)), "b"((addr)) : "memory")
#endif

#ifndef __write32
#define __write32(addr, val)                                            \
    __asm__ __volatile__("stw %0,0(%1)\n\teieio"                        \
        : : "r"((val)), "b"((addr)) : "memory")
#endif

#ifndef __write64
#define __write64(addr, val)                                            \
    __asm__ __volatile__("std %0,0(%1)\n\teieio"                        \
        : : "r"((val)), "b"((addr)) : "memory")
#endif

/*
 * __gettime() — read the 64-bit timebase.  `mftb` can transiently
 * read 0 across a low-word rollover boundary on some PPC parts; loop
 * until a non-zero sample is observed.  cr7 is clobbered by the
 * compare.
 */
#ifndef __gettime
#define __gettime() __extension__                                       \
    ({  register unsigned long long __tb;                               \
        __asm__ __volatile__(                                           \
            "1:\n\t"                                                    \
            "mftb  %[tb]\n\t"                                            \
            "cmpwi 7,%[tb],0\n\t"                                        \
            "beq-  7,1b"                                                 \
            : [tb] "=r"(__tb) : : "cr7");                                \
        __tb; })
#endif

/*
 * Byte-reverse helpers.  Provided only if the legacy <ppu-asm.h>
 * (guard __PPU_ASM_H__) has NOT already defined them; redefining a
 * static inline of the same name in one TU is a hard error, not a
 * warning, so this gate is mandatory rather than cosmetic.
 */
#if !defined(__PPU_ASM_H__) && !defined(_PS3DK_PPU_ASM_BSWAP_DEFINED_)
#define _PS3DK_PPU_ASM_BSWAP_DEFINED_ 1

#ifdef __cplusplus
extern "C" {
#endif

/*
 * `lhbrx` / `lwbrx` / `ldbrx` load a value with the bytes reversed;
 * feeding them the address of an in-register temp gives a
 * register-to-register byte swap.  `volatile` on the temp keeps the
 * store-then-byte-reversed-load from being optimized away.
 */
static inline unsigned short bswap16(unsigned short v)
{
    volatile unsigned short t = v;
    unsigned short r;
    __asm__ __volatile__("lhbrx %0,0,%1"
        : "=r"(r) : "b"(&t), "m"(t));
    return r;
}

static inline unsigned int bswap32(unsigned int v)
{
    volatile unsigned int t = v;
    unsigned int r;
    __asm__ __volatile__("lwbrx %0,0,%1"
        : "=r"(r) : "b"(&t), "m"(t));
    return r;
}

static inline unsigned long long bswap64(unsigned long long v)
{
    volatile unsigned long long t = v;
    unsigned long long r;
    __asm__ __volatile__("ldbrx %0,0,%1"
        : "=r"(r) : "b"(&t), "m"(t));
    return r;
}

#ifdef __cplusplus
}
#endif

#endif /* bswap gate */

#endif /* _PS3DK_SYS_PPU_ASM_H_ */
