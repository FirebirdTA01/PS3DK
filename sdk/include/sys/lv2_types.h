/*
 * sys/lv2_types.h — CellOS Lv-2 ABI-level type primitives.
 *
 * The PPU uses ELF64 with a 32-bit userland effective-address (EA)
 * space. Native C pointers (`void *`, `T *`) are 32-bit by default
 * (ILP32), or 64-bit with -mlp64. ABI-fixed EA fields remain 32-bit
 * in either data model and use an explicit `uint32_t`. This
 * header provides the typedef and conversion helpers that make that
 * distinction explicit, so future code neither loses upper bits by
 * accident nor conflates the two categories.
 *
 * See docs/abi/cellos-lv2-abi-spec.md section 4 for the normative
 * rules.
 *
 * Where to use:
 *   lv2_ea32_t       — any struct field or API parameter that carries
 *                      a 32-bit EA value. Examples: members of
 *                      .sys_proc_prx_param, sys_process_param_t's
 *                      crash_dump_param_addr, any value written to
 *                      an OPD entry-point slot.
 *
 *   void * / T *     — ordinary in-process pointers, whose width
 *                      follows the selected C data model. Fields
 *                      crossing the SPRX boundary must preserve
 *                      their fixed ABI width (see the spec above).
 *
 * Where NOT to use:
 *   Do not use lv2_ea32_t for general-purpose pointer storage.
 *   Do not declare `void *` fields with `__attribute__((mode(SI)))`
 *   in new code — that is the PSL1GHT workaround we are replacing.
 */

#ifndef _PS3DK_SYS_LV2_TYPES_H_
#define _PS3DK_SYS_LV2_TYPES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 32-bit effective address. Opaque to the caller — never dereferenced
 * directly. Convert in/out via lv2_ea32_pack / lv2_ea32_expand.
 */
typedef uint32_t lv2_ea32_t;

/*
 * Pack a native pointer into a 32-bit EA. In debug builds (non-NDEBUG),
 * traps if the pointer does not fit in 32 bits. This check is vacuous
 * for ILP32 and rejects nonzero upper bits for LP64.
 * Lv-2 userland EAs always fit in 32 bits; a failing assert means a
 * kernel-returned pointer or a cross-ABI value is being narrowed incorrectly.
 */
static inline lv2_ea32_t lv2_ea32_pack(const void *p)
{
    uintptr_t u = (uintptr_t)p;
#ifndef NDEBUG
    /* Fires on any value whose upper 32 bits aren't zero. Represents a
     * real bug in the caller — a non-userland pointer being stored in
     * a 32-bit ABI slot. */
    if (((uint64_t)u >> 32) != 0) {
        /* Pull in assert.h only when actually needed; keep the hot path
         * include-light. Using __builtin_trap keeps us freestanding. */
        __builtin_trap();
    }
#endif
    return (lv2_ea32_t)u;
}

/*
 * Expand a 32-bit EA into a native pointer. Zero-extends unconditionally;
 * never sign-extends. Safe for any well-formed Lv-2 EA.
 */
static inline void *lv2_ea32_expand(lv2_ea32_t ea)
{
    return (void *)(uintptr_t)(uint32_t)ea;
}

/*
 * Convert a C function pointer into the 32-bit EA the Lv-2 kernel
 * expects when a callback is being registered (VBlank / Flip / Queue
 * handlers, FIFO callback slots, etc).
 *
 * Under the native compact-OPD ABI, a C function pointer on PPU64 IS
 * the 32-bit EA of an 8-byte `.opd` descriptor `[entry_ea_32, toc_ea_32]`,
 * which is exactly what the kernel callback registry dereferences. So
 * this helper is a bare cast — its job is type discipline (return
 * `lv2_ea32_t` so EA-ness is explicit at every call site), not arithmetic.
 *
 * The retired transitional form added `+16` to address an env-slot
 * inside a 24-byte ELFv1 descriptor. That path is gone for native
 * output; `tools/sprx-linker` now only rewrites legacy 24-byte input
 * if any reaches the final image. See `docs/abi/compact-opd-migration.md`.
 */
static inline lv2_ea32_t lv2_fn_to_callback_ea(const void *fn)
{
    if (!fn) return 0;
    return (lv2_ea32_t)(uintptr_t)fn;
}

/*
 * Compile-time size + layout invariants. If these ever fail, the ABI
 * contract has drifted and the caller needs to re-read the spec doc.
 */
#ifdef _Static_assert
_Static_assert(sizeof(lv2_ea32_t) == 4, "lv2_ea32_t must be exactly 32 bits");
/* PS3DK supports the ELF64 + ILP32 hybrid ABI (4-byte pointers, default)
 * and the legacy ELF64 + LP64 ABI (8-byte pointers, opt-in via -mlp64).
 * SPRX cross-module pointers stay 4 bytes regardless via lv2_ea32_t /
 * ATTRIBUTE_PRXPTR; only the in-process pointer width differs. */
_Static_assert(sizeof(void *) == 4 || sizeof(void *) == 8,
	       "PPU ABI requires 4-byte (ILP32) or 8-byte (LP64) pointers");
#endif

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_SYS_LV2_TYPES_H_ */
