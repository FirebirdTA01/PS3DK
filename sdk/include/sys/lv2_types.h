/*
 * sys/lv2_types.h — CellOS Lv-2 ABI-level type primitives.
 *
 * The Lv-2 ABI is 64-bit but has a 32-bit userland effective-address
 * (EA) space. The public API exposes 64-bit native C pointers
 * (`void *`, `T *`) for application use, but a small number of ABI-
 * fixed fields carry a 32-bit EA as an explicit `uint32_t`. This
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
 *   void * / T *     — everything else. User-visible pointers in
 *                      public struct fields (CellGcmConfig, etc.)
 *                      are native 64-bit pointers, per Lv-2 ABI.
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
 * asserts that the pointer fits in 32 bits — i.e. its upper half is zero.
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
    if ((u >> 32) != 0) {
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
 * Compile-time size + layout invariants. If these ever fail, the ABI
 * contract has drifted and the caller needs to re-read the spec doc.
 */
#ifdef _Static_assert
_Static_assert(sizeof(lv2_ea32_t) == 4, "lv2_ea32_t must be exactly 32 bits");
_Static_assert(sizeof(void *) == 8, "PPU ABI requires 64-bit native pointers");
#endif

#ifdef __cplusplus
}
#endif

#endif /* _PS3DK_SYS_LV2_TYPES_H_ */
