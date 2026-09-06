/* NV40TCL_FP_CONTROL: the ONE builder of the fragment-program control word.
 *
 * Three consumers bind fragment programs - cellGcmSetFragmentProgram (a
 * CGprogram container), cellGcmSetFragmentProgramLoad (a CellCgb
 * configuration) and PSGL's bootstrap (also a CellCgb configuration) - and
 * until 2026-09-06 each carried its own copy of this arithmetic.  The copies
 * agreed with each other and were all wrong the same way: none forwarded the
 * container's depthReplace flag, so every depth export through this SDK was
 * discarded by the RSX (t_6305f2f6).  A fix landed in one copy and missed
 * another in the same file, which is the defect class duplication creates.
 * Hence one pure function, host-testable (tests/sdk/fp-control-word-test.sh
 * pins every word below), with no dependency beyond <stdint.h>.
 *
 * Bit facts, each with its provenance:
 *   0x40      output from R0, 32-bit exports        (PSL1GHT cgcomp; every
 *                                                     container measured)
 *   0x0e      DEPTH EXPORT                           (MEASURED on the
 *             differential rig 2026-09-06: two reference programs exporting
 *             depths 0.125 apart read exactly 2097152 Z24 units apart on
 *             4096 px once this bit is set, and the vertex program's Z
 *             without it; RPCS3 decodes it as SHADER_CONTROL_DEPTH_EXPORT)
 *   0x0e      "output from H0"                       (PSL1GHT lore, UNTESTED:
 *             125 containers scanned and not one sets outputFromH0, so that
 *             branch has never executed for anything measured.  The same
 *             value as DEPTH EXPORT: with H0 output, depth export cannot be
 *             expressed independently.  Owed a zeta-judged H0 fixture,
 *             t_96daf53b; until it lands the H0 branch is kept as it was.)
 *   1 << 7    program uses KIL
 *   1 << 10   program enable - without it the FP never executes
 *   n << 24   temp register count, NV40 minimum 2
 */
#ifndef PS3TC_CELL_GCM_FP_CONTROL_H
#define PS3TC_CELL_GCM_FP_CONTROL_H

#include <stdint.h>

#define PS3TC_FP_CONTROL_OUTPUT_R0     0x40u
#define PS3TC_FP_CONTROL_OUTPUT_H0     0x0eu   /* PSL1GHT lore, untested */
#define PS3TC_FP_CONTROL_DEPTH_EXPORT  0x0eu   /* measured */
#define PS3TC_FP_CONTROL_KIL           (1u << 7)
#define PS3TC_FP_CONTROL_ENABLE        (1u << 10)
#define PS3TC_FP_CONTROL_REGS_SHIFT    24
#define PS3TC_FP_CONTROL_MIN_REGS      2u

/* libcgb packs the container's header flags into
 * CellCgbFragmentProgramConfiguration.fragmentControl (cgb_parse.c):
 * partialTexType in the low 16 bits, then one bit each. */
#define PS3TC_CGB_FRAGMENT_CONTROL_OUTPUT_FROM_H0  (1u << 16)
#define PS3TC_CGB_FRAGMENT_CONTROL_DEPTH_REPLACE   (1u << 17)
#define PS3TC_CGB_FRAGMENT_CONTROL_PIXEL_KILL      (1u << 18)

static inline uint32_t ps3tc_fp_control_word(uint32_t output_from_h0,
                                             uint32_t depth_replace,
                                             uint32_t pixel_kill,
                                             uint32_t register_count)
{
    const uint32_t num_regs = (register_count > PS3TC_FP_CONTROL_MIN_REGS)
                            ? register_count : PS3TC_FP_CONTROL_MIN_REGS;
    uint32_t low = output_from_h0 ? PS3TC_FP_CONTROL_OUTPUT_H0
                                  : PS3TC_FP_CONTROL_OUTPUT_R0;
    if (depth_replace) low |= PS3TC_FP_CONTROL_DEPTH_EXPORT;
    if (pixel_kill)    low |= PS3TC_FP_CONTROL_KIL;
    return low | PS3TC_FP_CONTROL_ENABLE
               | (num_regs << PS3TC_FP_CONTROL_REGS_SHIFT);
}

static inline uint32_t ps3tc_fp_control_from_cgb(uint32_t fragment_control,
                                                 uint32_t register_count)
{
    return ps3tc_fp_control_word(
        (fragment_control & PS3TC_CGB_FRAGMENT_CONTROL_OUTPUT_FROM_H0) ? 1u : 0u,
        (fragment_control & PS3TC_CGB_FRAGMENT_CONTROL_DEPTH_REPLACE)  ? 1u : 0u,
        (fragment_control & PS3TC_CGB_FRAGMENT_CONTROL_PIXEL_KILL)     ? 1u : 0u,
        register_count);
}

#endif /* PS3TC_CELL_GCM_FP_CONTROL_H */
