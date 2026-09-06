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
 * Bit facts, each with its provenance.  The two output-related facts were
 * SETTLED by measurement on 2026-09-06 (t_96daf53b) and are not what the
 * PSL1GHT-derived lore said:
 *   0x40      32-BIT EXPORTS - the colour is read from R0.  Its ABSENCE is
 *             what selects the half output register: an fp16-output program
 *             bound with NO low bits at all painted exactly the colour its
 *             fp32 twin did, on 4096 pixels (TTY-h0-zero2).  So "output from
 *             H0" is not a bit you set, it is a bit you leave clear.
 *   0x0e      DEPTH EXPORT, and NOTHING ELSE.  Measured twice: two reference
 *             programs exporting depths 0.125 apart read exactly 2097152 Z24
 *             units apart on 4096 px once this bit is set (2026-09-06), and
 *             an H0-output program declaring NO depth exported R1.z as a
 *             per-row ramp while this bit was set for it (TTY-h0-v1) and read
 *             the vertex program's Z once it was not (TTY-h0-zero2).  RPCS3
 *             decodes it as SHADER_CONTROL_DEPTH_EXPORT.
 *
 *             PSL1GHT's cgcomp binds 0x0e for "output from H0".  That is
 *             WRONG BY THREE BITS, not merely unproven: it selects nothing
 *             and it turns on depth export, so every half-output fragment
 *             program bound through a PSL1GHT-derived SDK - ours included
 *             until 2026-09-06 - wrote a depth the shader never asked for,
 *             taken from whatever R1.z happened to hold.  An H0 output
 *             therefore binds 0x00 here, and depth is added only when the
 *             container declares depthReplace.
 *
 *             Whether 0x0e is a single enable or three bits with separate
 *             meanings is not decided, and does not need to be: everything
 *             we must express is expressible.  It is on the hardware
 *             spot-check list with t_26e7fa11.  The measurements above are
 *             RPCS3's behaviour; hardware confirmation is owed for both.
 *   1 << 7    program uses KIL
 *   1 << 10   program enable - without it the FP never executes
 *   n << 24   temp register count, NV40 minimum 2
 */
#ifndef PS3TC_CELL_GCM_FP_CONTROL_H
#define PS3TC_CELL_GCM_FP_CONTROL_H

#include <stdint.h>

#define PS3TC_FP_CONTROL_OUTPUT_R0     0x40u
#define PS3TC_FP_CONTROL_OUTPUT_H0     0x00u   /* measured: H0 = no 0x40  */
#define PS3TC_FP_CONTROL_DEPTH_EXPORT  0x0eu   /* measured: depth only    */
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
