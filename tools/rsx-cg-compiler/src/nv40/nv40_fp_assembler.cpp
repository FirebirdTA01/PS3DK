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
    lastInstrOffset_ = base;
    hasInstruction_  = true;
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

void FpAssembler::emitFencbr()
{
    const size_t base = logicalWords_.size();
    logicalWords_.resize(base + 4, 0u);
    lastInstrOffset_ = base;
    hasInstruction_  = true;
    uint32_t* hw = &logicalWords_[base];

    // OPCODE = 0x3E (RSX extension, "fence before read").
    hw[0] |= (uint32_t{0x3Eu} << NVFX_FP_OP_OPCODE_SHIFT);
    // OUT_NONE: no destination written.
    hw[0] |= NV40_FP_OP_OUT_NONE;
    // sce-cgc sets OUT_REG = 0x3F (sentinel bit pattern when OUT_NONE
    // is set — the field is otherwise meaningless).
    hw[0] |= (uint32_t{0x3Fu} << NVFX_FP_OP_OUT_REG_SHIFT);
    // OUTMASK = 0xF — all channels (also meaningless with OUT_NONE,
    // but sce-cgc emits it this way and we match byte-for-byte).
    hw[0] |= (uint32_t{0xFu}  << NVFX_FP_OP_OUTMASK_SHIFT);

    // Source operands are default: TYPE=TEMP(0), identity swizzle,
    // cond_cond=TR, identity cond_swz.  Mirrors what emitSrc writes
    // for NVFXSR_NONE — same bits via a direct hw[] write.
    hw[1] = (uint32_t{NVFX_COND_TR} << NVFX_FP_OP_COND_SHIFT);
    hw[1] |= ((uint32_t{0} << NVFX_FP_OP_COND_SWZ_X_SHIFT) |
              (uint32_t{1} << NVFX_FP_OP_COND_SWZ_Y_SHIFT) |
              (uint32_t{2} << NVFX_FP_OP_COND_SWZ_Z_SHIFT) |
              (uint32_t{3} << NVFX_FP_OP_COND_SWZ_W_SHIFT));
    // SRC0 common: TYPE_TEMP(0) + identity swz (0,1,2,3).
    const uint32_t defaultSrc =
        (NVFX_FP_REG_TYPE_TEMP << NVFX_FP_REG_TYPE_SHIFT) |
        (uint32_t{0} << NVFX_FP_REG_SWZ_X_SHIFT) |
        (uint32_t{1} << NVFX_FP_REG_SWZ_Y_SHIFT) |
        (uint32_t{2} << NVFX_FP_REG_SWZ_Z_SHIFT) |
        (uint32_t{3} << NVFX_FP_REG_SWZ_W_SHIFT);
    hw[1] |= defaultSrc;
    hw[2] = defaultSrc;
    hw[3] = defaultSrc;
}

void FpAssembler::appendConstBlock(const float values[4])
{
    // Inline constants are 4×fp32 placed immediately after the
    // using instruction.  They undergo the same halfword swap as
    // ucode words at words() time, so store them here in the
    // caller's natural (big-endian-float) layout.
    uint32_t raw[4];
    std::memcpy(raw, values, 16);
    logicalWords_.push_back(raw[0]);
    logicalWords_.push_back(raw[1]);
    logicalWords_.push_back(raw[2]);
    logicalWords_.push_back(raw[3]);
    // Deliberately do NOT update lastInstrOffset_ — this block is
    // data, not an instruction.  markEnd() should stamp PROGRAM_END
    // on the real instruction that came before.
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
    case NVFXSR_CONST:
        // Inline FP constants live in a 16-byte block placed right
        // after the using instruction.  TYPE_CONST selects the const
        // bank; SRC-index=0 references the single inline slot.
        // (Multiple consts in one shader use distinct blocks; we
        // encode that by emitting them sequentially and each MOV
        // points to its own following block via SRC-index 0 — the
        // "current instruction's own inline" — per sce-cgc output.)
        sr |= (NVFX_FP_REG_TYPE_CONST << NVFX_FP_REG_TYPE_SHIFT);
        sr |= (static_cast<uint32_t>(src.reg.index) << NVFX_FP_REG_SRC_SHIFT);
        break;
    case NVFXSR_NONE:
        sr |= (NVFX_FP_REG_TYPE_TEMP << NVFX_FP_REG_TYPE_SHIFT);
        break;
    default:
        // IMM / SAMPLER land in later stages; treat as TEMP
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
    if (!hasInstruction_) return;
    logicalWords_[lastInstrOffset_] |= NVFX_FP_OP_PROGRAM_END;
}

std::vector<uint32_t> FpAssembler::words() const
{
    std::vector<uint32_t> out(logicalWords_.size());
    for (size_t i = 0; i < logicalWords_.size(); ++i)
        out[i] = halfswap(logicalWords_[i]);
    return out;
}

}  // namespace nv40
