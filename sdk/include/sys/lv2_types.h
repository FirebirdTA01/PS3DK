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
 * Convert a C function pointer into the 32-bit EA the Lv-2 kernel
 * expects when a callback is being registered (VBlank / Flip / Queue
 * handlers, FIFO callback slots, etc).
 *
 * Mechanism (current, transitional): on ELFv1 PPU64 the C `&func`
 * yields a pointer to a 24-byte function descriptor. The kernel's
 * callback registry dereferences the value it is given to read an
 * 8-byte compact descriptor `[entry_ea_32, toc_ea_32]`. Our post-
 * link step (tools/sprx-linker) writes that 8-byte block into the
 * env slot at offset +16 of the 24-byte descriptor — it is
 * pre-packed and waiting. So the correct kernel-facing EA is
 * `&func + 16`.
 *
 * This is a typed, documented replacement for PSL1GHT's
 * `__get_opd32` macro. Behaviour is byte-identical; what changes is
 * that the return type is `lv2_ea32_t`, making the EA-ness of the
 * value explicit at every call site.
 *
 * Future state: when GCC emits a 2-read indirect-call sequence and
 * binutils emits 8-byte `.opd` entries natively, a C function
 * pointer IS the 32-bit descriptor EA already. This helper then
 * becomes a bare cast with no +16, and every caller keeps the same
 * source. See `docs/abi/compact-opd-migration.md`.
 */
static inline lv2_ea32_t lv2_fn_to_callback_ea(const void *fn)
{
    if (!fn) return 0;
    return (lv2_ea32_t)(uintptr_t)((const uint8_t *)fn + 16);
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
