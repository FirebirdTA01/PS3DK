/*
 * NV40 fragment-program assembler.
 *
 * Port of PSL1GHT cgcomp/source/compilerfp.cpp's emit_insn / emit_dst /
 * emit_src into a standalone, allocator-free module.  Differences from
 * the PSL1GHT path:
 *
 *  - Output buffer kept in *logical* bit layout; halfword-swap applied
 *    at words() so `out.words` matches sce-cgc's on-disk encoding.
 *  - The 0x7fc << NV40_FP_OP_ADDR_INDEX_SHIFT default on hw[3] is
 *    written for ANY varying-input read, not just TC inputs.
 *    PSL1GHT only does it for TC because it gates perspective-correct
 *    interpolation; sce-cgc applies it across the board (verified on
 *    identity_f.cg's MOVR R0, f[COL0] — hw[3] = 0x3fe1c800).
 *
 *  Other FP features (constants, samplers, branching, condition codes,
 *  precision modes, alphakill) land as real shaders need them.
 */

#include "nv40_fp_assembler.h"

#include "nvfx_shader.h"

namespace nv40
{

namespace
{
inline uint32_t halfswap(uint32_t w) { return (w >> 16) | (w << 16); }
}

void FpAssembler::emit(const struct nvfx_insn& insn, uint8_t opcode)
{
    const size_t base = logicalWords_.size();
    logicalWords_.resize(base + 4, 0u);
    uint32_t* hw = &logicalWords_[base];

    hw[0] |= (static_cast<uint32_t>(opcode) << NVFX_FP_OP_OPCODE_SHIFT);
    hw[0] |= (static_cast<uint32_t>(insn.mask) << NVFX_FP_OP_OUTMASK_SHIFT);
    hw[0] |= (static_cast<uint32_t>(insn.precision) << NVFX_FP_OP_PRECISION_SHIFT);

    // TEX/TXP/TXB/TXD/TXL all carry a tex-unit index in bits 17-20 of
    // hw[0].  Caller sets insn.tex_unit >= 0 only for those opcodes;
    // arithmetic ops leave it at -1 and we skip the field.
    if (insn.tex_unit >= 0)
        hw[0] |= (static_cast<uint32_t>(insn.tex_unit) << NVFX_FP_OP_TEX_UNIT_SHIFT);

    if (insn.sat)
        hw[0] |= NVFX_FP_OP_OUT_SAT;

    if (insn.cc_update)
        hw[0] |= NVFX_FP_OP_COND_WRITE_ENABLE;

    hw[1] |= (static_cast<uint32_t>(insn.cc_cond) << NVFX_FP_OP_COND_SHIFT);
    hw[1] |= ((static_cast<uint32_t>(insn.cc_swz[0]) << NVFX_FP_OP_COND_SWZ_X_SHIFT) |
              (static_cast<uint32_t>(insn.cc_swz[1]) << NVFX_FP_OP_COND_SWZ_Y_SHIFT) |
              (static_cast<uint32_t>(insn.cc_swz[2]) << NVFX_FP_OP_COND_SWZ_Z_SHIFT) |
              (static_cast<uint32_t>(insn.cc_swz[3]) << NVFX_FP_OP_COND_SWZ_W_SHIFT));

    emitDst(insn, hw);
    emitSrc(insn, 0, hw);
    emitSrc(insn, 1, hw);
    emitSrc(insn, 2, hw);
}

void FpAssembler::emitDst(const struct nvfx_insn& insn, uint32_t* hw)
{
    const struct nvfx_reg& dst = insn.dst;
    const int32_t index = dst.index;

    switch (dst.type)
    {
    case NVFXSR_TEMP:
    {
        const int hwReg = dst.is_fp16 ? (index >> 1) : index;
        if (numTempRegs_ < hwReg + 1)
            numTempRegs_ = hwReg + 1;
        break;
    }
    case NVFXSR_OUTPUT:
        // R0 is result.color, R1 is result.depth.  FPControl bit
        // tracking (DEPTH_USE / KIL_USE) lands when those features
        // do — identity_f only writes R0.
        break;
    case NVFXSR_NONE:
        hw[0] |= NV40_FP_OP_OUT_NONE;
        break;
    default:
        break;
    }

    if (dst.is_fp16)
        hw[0] |= NVFX_FP_OP_OUT_REG_HALF;

    hw[0] |= (static_cast<uint32_t>(index) << NVFX_FP_OP_OUT_REG_SHIFT);
}

void FpAssembler::emitSrc(const struct nvfx_insn& insn, int pos, uint32_t* hw)
{
    uint32_t sr = 0;
    const struct nvfx_src& src = insn.src[pos];

    switch (src.reg.type)
    {
    case NVFXSR_INPUT:
        sr |= (NVFX_FP_REG_TYPE_INPUT << NVFX_FP_REG_TYPE_SHIFT);
        // INPUT_SRC selects which interpolated varying feeds this
        // instruction; it's a per-insn field, not per-slot.
        hw[0] |= (static_cast<uint32_t>(src.reg.index) << NVFX_FP_OP_INPUT_SRC_SHIFT);
        // sce-cgc's default for hw[3] when reading any varying:
        // (0x7fc << ADDR_INDEX_SHIFT) | (disable_pc << 31).
        // PSL1GHT only sets this for TC inputs, but sce-cgc applies it
        // unconditionally — see REVERSE_ENGINEERING.md.
        hw[3] |= ((static_cast<uint32_t>(insn.disable_pc) << NV40_FP_OP_DISABLE_PC_SHIFT) |
                  (0x7fcu << NV40_FP_OP_ADDR_INDEX_SHIFT));
        break;
    case NVFXSR_TEMP:
        sr |= (NVFX_FP_REG_TYPE_TEMP << NVFX_FP_REG_TYPE_SHIFT);
        sr |= (static_cast<uint32_t>(src.reg.index) << NVFX_FP_REG_SRC_SHIFT);
        break;
    case NVFXSR_NONE:
        sr |= (NVFX_FP_REG_TYPE_TEMP << NVFX_FP_REG_TYPE_SHIFT);
        break;
    default:
        // CONST / IMM / SAMPLER land in later stages; treat as TEMP
        // placeholder so we don't write garbage type bits.
        sr |= (NVFX_FP_REG_TYPE_TEMP << NVFX_FP_REG_TYPE_SHIFT);
        break;
    }

    if (src.reg.is_fp16) sr |= NVFX_FP_REG_SRC_HALF;
    if (src.negate)      sr |= NVFX_FP_REG_NEGATE;
    if (src.abs)
    {
        // Absolute-value flag is per source position, in the high
        // bits of hw[pos+1] (NOT in the common-source `sr` mask).
        if      (pos == 2) hw[3] |= NVFX_FP_OP_SRC2_ABS;
        else if (pos == 1) hw[2] |= NVFX_FP_OP_SRC1_ABS;
        else               hw[1] |= NVFX_FP_OP_SRC0_ABS;
    }

    sr |= ((static_cast<uint32_t>(src.swz[0]) << NVFX_FP_REG_SWZ_X_SHIFT) |
           (static_cast<uint32_t>(src.swz[1]) << NVFX_FP_REG_SWZ_Y_SHIFT) |
           (static_cast<uint32_t>(src.swz[2]) << NVFX_FP_REG_SWZ_Z_SHIFT) |
           (static_cast<uint32_t>(src.swz[3]) << NVFX_FP_REG_SWZ_W_SHIFT));

    hw[pos + 1] |= sr;
}

void FpAssembler::markEnd()
{
    if (logicalWords_.empty()) return;
    const size_t base = logicalWords_.size() - 4;
    logicalWords_[base] |= NVFX_FP_OP_PROGRAM_END;
}

std::vector<uint32_t> FpAssembler::words() const
{
    std::vector<uint32_t> out(logicalWords_.size());
    for (size_t i = 0; i < logicalWords_.size(); ++i)
        out[i] = halfswap(logicalWords_[i]);
    return out;
}

}  // namespace nv40
