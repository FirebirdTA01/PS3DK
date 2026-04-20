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

    // Emit an RSX FENCBR instruction — opcode 0x3E, OUT_NONE = 1,
    // OUT_REG = 0x3F (sentinel bit pattern used by the reference
    // compiler), all source operands default.  Placed before any
    // instruction that reads an inline-const block to stall the
    // pipeline until the const is available.  Not in the NV40 opcode
    // header — verified from reference-compiler output.
    void emitFencbr();

    // Append a 16-byte inline constant immediately after the most
    // recent instruction — matches sce-cgc's layout for FP literal
    // consts.  Values are four fp32s in the caller's natural order
    // (value[0] is the X lane etc.); the on-disk halfword swap is
    // applied at words() like for instructions.
    void appendConstBlock(const float values[4]);

    // Stamp the NVFX_FP_OP_PROGRAM_END bit on the most recent
    // *instruction* (not a const block).  Must be called exactly
    // once after the whole program is emitted.
    void markEnd();

    // Returns the ucode in on-disk halfword-swapped form (matches
    // sce-cgc output and what RSX expects in the .fpo blob).
    std::vector<uint32_t> words() const;

    int      numTempRegs()     const { return numTempRegs_; }
    bool     empty()           const { return logicalWords_.empty(); }
    // Current buffer size in bytes — used by the lowering pass to
    // record CgBinaryEmbeddedConstant ucode offsets just before
    // calling appendConstBlock().
    uint32_t currentByteSize() const
    { return static_cast<uint32_t>(logicalWords_.size() * 4u); }

private:
    // Logical bit layout (matches NVFX_FP_OP_* shifts/masks).
    // 4 u32 per instruction-or-const-block; the disk swap happens
    // in words() for both.
    std::vector<uint32_t> logicalWords_;

    // Offset of the most recent *instruction* (not const block).
    // markEnd() stamps PROGRAM_END here, so inline const blocks that
    // happen to be the last entry in logicalWords_ don't get
    // corrupted.
    size_t lastInstrOffset_ = 0;
    bool   hasInstruction_  = false;

    // R0 is implicitly reserved for result.color, so the live count
    // starts at 1 even if no temp is explicitly written.
    int numTempRegs_ = 1;

    void emitDst(const struct nvfx_insn& insn, uint32_t* hw);
    void emitSrc(const struct nvfx_insn& insn, int pos, uint32_t* hw);
};

}  // namespace nv40

#endif  /* RSX_CG_COMPILER_NV40_FP_ASSEMBLER_H */
