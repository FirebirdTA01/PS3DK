#ifndef RSX_CG_COMPILER_NV40_VP_ASSEMBLER_H
#define RSX_CG_COMPILER_NV40_VP_ASSEMBLER_H

/*
 * Minimal NV40 vertex-program assembler.
 *
 * Wraps Mesa/Nouveau's nvfx_insn → 128-bit-word packing.  Ported from
 * PSL1GHT cgcomp/source/compilervp.cpp with the class state reduced to
 * the subset we actually need:
 *     - dynamic instruction buffer (flat u32 stream, 4 words per insn)
 *     - input / output bitmasks (for container metadata)
 *     - NVFX_VP_INST_LAST stamp on the final instruction
 *
 * Relocations, merged vec+sca slots, condition codes, branching, and
 * the temp/const register allocator get added as real Cg programs
 * need them.
 */

#include <cstdint>
#include <vector>

struct nvfx_insn;  // defined in nvfx_shader.h; forward-decl to keep this header light

namespace nv40
{

class VpAssembler
{
public:
    // Emit a vector-slot instruction.  `opcode` comes from the PSL1GHT
    // gen_op(o,VEC) macro — i.e. (slot<<7) | NVFX_VP_INST_VEC_OP_<o>.
    // The assembler handles dst/src field packing and slot selection.
    void emit(const struct nvfx_insn& insn, uint8_t opcode);

    // Emit one instruction word containing both a VEC-slot op and a
    // SCA-slot op.  This is explicit only; callers use it where an oracle
    // schedule requires co-issue, so existing shaders are not rescheduled
    // heuristically.
    void emitCoIssued(const struct nvfx_insn& vecInsn, uint8_t vecOpcode,
                      const struct nvfx_insn& scaInsn, uint8_t scaOpcode);

    // Stamp the NVFX_VP_INST_LAST bit on the most recent instruction.
    // Must be called exactly once after the whole program is emitted.
    void markLast();

    // Flatten instruction buffer to a contiguous u32 stream.
    const std::vector<uint32_t>& words() const { return words_; }

    uint32_t inputMask () const { return inputMask_;  }

    // NV40 VP instructions have ONE per-instruction input-register
    // field.  Encoding two DISTINCT input sources OR-ed their indices
    // (9|10=11) into a register neither operand named, silently.  The
    // assembler cannot fix that - only a lowering can materialise the
    // second input into a temp - so it RECORDS the conflict and the
    // caller must refuse to emit the program.
    bool inputConflict() const { return inputConflict_; }
    uint32_t outputMask() const { return outputMask_; }
    int      numTempRegs() const { return numTempRegs_; }

    bool empty() const { return words_.empty(); }

private:
    std::vector<uint32_t> words_;          // 4 u32 per instruction
    uint32_t              inputMask_   = 0;
    uint32_t              outputMask_  = 0;
    int                   numTempRegs_ = 0;
    bool                  inputConflict_ = false;

    // Returns the base offset of the current instruction inside words_.
    size_t curBase() const { return words_.size() - 4; }
    uint32_t* curData() { return &words_[curBase()]; }

    void emitDst(const struct nvfx_insn& insn, uint32_t slot);
    void emitSrc(const struct nvfx_insn& insn, uint8_t pos);
};

}  // namespace nv40

#endif  /* RSX_CG_COMPILER_NV40_VP_ASSEMBLER_H */
