#ifndef RSX_CG_COMPILER_NV40_FP_ASSEMBLER_H
#define RSX_CG_COMPILER_NV40_FP_ASSEMBLER_H

/*
 * Minimal NV40 fragment-program assembler.
 *
 * Mirrors VpAssembler's contract but produces NV40 FP ucode.  Two
 * differences from the vertex side worth knowing about:
 *
 * 1.  Fragment ucode is stored HALFWORD-SWAPPED on disk relative to
 *     the NVFX_FP_OP_* bit layout.  This class keeps its internal
 *     buffer in *logical* form (matching nvfx_shader.h's NVFX_FP_OP_*
 *     defines) and applies the swap in `words()`.  See
 *     docs/REVERSE_ENGINEERING.md "Fragment profile" section.
 *
 * 2.  Inputs are read via a per-instruction INPUT_SRC field in hw[0]
 *     rather than per-source-slot register IDs.  An FP instruction
 *     can therefore have at most one INPUT-bank source.
 *
 * Field packing is ported from PSL1GHT cgcomp/source/compilerfp.cpp's
 * emit_insn / emit_dst / emit_src — see REVERSE_ENGINEERING.md for the
 * delta against sce-cgc (notably the unconditional 0x7fc << 19 default
 * on hw[3] for any varying-input read).
 */

#include <cstdint>
#include <vector>

struct nvfx_insn;

namespace nv40
{

class FpAssembler
{
public:
    // Emit one fragment-program instruction.  `opcode` is the raw
    // NVFX_FP_OP_OPCODE_* value (e.g. NVFX_FP_OP_OPCODE_MOV).
    void emit(const struct nvfx_insn& insn, uint8_t opcode);

    // Stamp the NVFX_FP_OP_PROGRAM_END bit on the most recent
    // instruction's hw[0].  Must be called exactly once after the
    // whole program is emitted.
    void markEnd();

    // Returns the ucode in on-disk halfword-swapped form (matches
    // sce-cgc output and what RSX expects in the .fpo blob).
    std::vector<uint32_t> words() const;

    int  numTempRegs() const { return numTempRegs_; }
    bool empty()       const { return logicalWords_.empty(); }

private:
    // Logical bit layout (matches NVFX_FP_OP_* shifts/masks).
    // 4 u32 per instruction; the disk swap happens in words().
    std::vector<uint32_t> logicalWords_;

    // R0 is implicitly reserved for result.color, so the live count
    // starts at 1 even if no temp is explicitly written.
    int numTempRegs_ = 1;

    void emitDst(const struct nvfx_insn& insn, uint32_t* hw);
    void emitSrc(const struct nvfx_insn& insn, int pos, uint32_t* hw);
};

}  // namespace nv40

#endif  /* RSX_CG_COMPILER_NV40_FP_ASSEMBLER_H */
