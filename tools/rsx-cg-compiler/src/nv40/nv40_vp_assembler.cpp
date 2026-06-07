/*
 * NV40 vertex-program assembler.
 *
 * Port of PSL1GHT cgcomp/source/compilervp.cpp's emit_insn / emit_dst /
 * emit_src into a standalone, allocator-free module.  The original lives
 * inside CCompilerVP with relocation lists, merged slot handling, temp
 * allocator, etc.; we carry only what's needed to emit VEC-slot ops with
 * INPUT / TEMP / OUTPUT banks — enough for the identity MOV path.  The
 * remaining encodings (CONST relocations, SCA merged slots, samplers,
 * branching) land when real shaders require them.
 */

#include "nv40_vp_assembler.h"

#include "nv40_vertprog.h"
#include "nv30_vertprog.h"
#include "nvfx_shader.h"

#include <array>
#include <cassert>
#include <cstring>

namespace nv40
{

void VpAssembler::emit(const struct nvfx_insn& insn, uint8_t opcode)
{
    const uint32_t slot = static_cast<uint32_t>(opcode) >> 7;
    const uint32_t op   = static_cast<uint32_t>(opcode) & 0x7f;

    const size_t base = words_.size();
    words_.resize(base + 4, 0u);  // zero-initialise new 128-bit insn

    // emit_dst / emit_src read hw[] in place, so we hold a pointer that
    // stays valid for the scope of this call (no further resizes below).
    uint32_t* hw = &words_[base];

    // --- dst ---
    {
        const struct nvfx_reg& dst = insn.dst;
        switch (dst.type)
        {
        case NVFXSR_NONE:
            hw[3] |= NV40_VP_INST_DEST_MASK;
            if (slot == 0)
                hw[0] |= NV40_VP_INST_VEC_DEST_TEMP_MASK;
            else
                hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
            break;

        case NVFXSR_ADDRESS:
        case NVFXSR_TEMP:
            if (numTempRegs_ < (dst.index + 1))
                numTempRegs_ = dst.index + 1;
            hw[3] |= NV40_VP_INST_DEST_MASK;
            if (slot == 0)
                hw[0] |= (dst.index << NV40_VP_INST_VEC_DEST_TEMP_SHIFT);
            else
                hw[3] |= (dst.index << NV40_VP_INST_SCA_DEST_TEMP_SHIFT);
            break;

        case NVFXSR_OUTPUT:
        {
            // Container `attributeOutputMask`: the reference compiler encodes one bit
            // per *front-face* output, with HPOS implicit (= no bit).
            // Verified 2026-04-18 against identity_v / identity_color_v
            // / mvp_passthrough_v / probe_tc_v .vpo dumps.
            //
            // PSL1GHT cgcomp also sets a "back-face" bit for COL/BFC
            // (doubling 0x01 → 0x05 for COL0, etc.); the reference compiler does not,
            // so we don't either.  HPOS contributes 0.
            switch (dst.index)
            {
            case NV40_VP_INST_DEST_COL0:
            case NV40_VP_INST_DEST_COL1:
                outputMask_ |= (1u << (dst.index - NV40_VP_INST_DEST_COL0));
                break;
            case NV40_VP_INST_DEST_BFC0:
            case NV40_VP_INST_DEST_BFC1:
                outputMask_ |= (4u << (dst.index - NV40_VP_INST_DEST_BFC0));
                break;
            case NV40_VP_INST_DEST_FOGC: outputMask_ |= (1u << 4); break;
            case NV40_VP_INST_DEST_PSZ : outputMask_ |= (1u << 5); break;
            default:
                if (dst.index >= NV40_VP_INST_DEST_TC(0) &&
                    dst.index <= NV40_VP_INST_DEST_TC(7))
                {
                    outputMask_ |= (0x4000u << (dst.index - NV40_VP_INST_DEST_TC0));
                }
                break;
            }
            hw[3] |= (dst.index << NV40_VP_INST_DEST_SHIFT);
            if (slot == 0)
            {
                hw[0] |= NV40_VP_INST_VEC_RESULT;
                hw[0] |= NV40_VP_INST_VEC_DEST_TEMP_MASK;
            }
            else
            {
                hw[3] |= NV40_VP_INST_SCA_RESULT;
                hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
            }
            break;
        }
        }
    }

    // --- srcs ---
    for (uint8_t pos = 0; pos < 3; ++pos)
    {
        const struct nvfx_src& src = (slot == NVFX_VP_INST_SLOT_SCA)
            ? (pos == 2 ? insn.src[0] : insn.src[pos == 0 ? 2 : 1])
            : insn.src[pos];
        uint32_t sr = 0;

        switch (src.reg.type)
        {
        case NVFXSR_TEMP:
            sr |= (NVFX_VP(SRC_REG_TYPE_TEMP) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            sr |= (src.reg.index << NVFX_VP(SRC_TEMP_SRC_SHIFT));
            break;
        case NVFXSR_INPUT:
            sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            inputMask_ |= (1u << src.reg.index);
            hw[1] |= (src.reg.index << NVFX_VP(INST_INPUT_SRC_SHIFT));
            break;
        case NVFXSR_CONST:
            // CONST_SRC is a 9-bit field at bits 12..20 of hw[1] (not 8 as
            // the PSL1GHT-ported header suggests — the reference compiler writes the full
            // absolute c[] register index here, 0..511).
            sr |= (NVFX_VP(SRC_REG_TYPE_CONST) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            hw[1] |= (static_cast<uint32_t>(src.reg.index) & 0x1FF)
                     << NV40_VP_INST_CONST_SRC_SHIFT;
            if (src.indirect)
            {
                hw[3] |= NV40_VP_INST_INDEX_CONST;
                if (src.indirect_reg)
                    hw[0] |= NV40_VP_INST_ADDR_REG_SELECT_1;
                hw[0] |= (static_cast<uint32_t>(src.indirect_swz) & 0x3u)
                         << NV40_VP_INST_ADDR_SWZ_SHIFT;
            }
            break;
        case NVFXSR_NONE:
            sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            break;
        default:
            sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            break;
        }

        if (src.negate) sr |= NVFX_VP(SRC_NEGATE);
        if (src.abs)    hw[0] |= (1u << (21 + pos));

        sr |= ((src.swz[0] << NVFX_VP(SRC_SWZ_X_SHIFT)) |
               (src.swz[1] << NVFX_VP(SRC_SWZ_Y_SHIFT)) |
               (src.swz[2] << NVFX_VP(SRC_SWZ_Z_SHIFT)) |
               (src.swz[3] << NVFX_VP(SRC_SWZ_W_SHIFT)));

        switch (pos)
        {
        case 0:
            hw[1] |= (((sr & NVFX_VP(SRC0_HIGH_MASK)) >> NVFX_VP(SRC0_HIGH_SHIFT))
                      << NVFX_VP(INST_SRC0H_SHIFT));
            hw[2] |= ((sr & NVFX_VP(SRC0_LOW_MASK)) << NVFX_VP(INST_SRC0L_SHIFT));
            break;
        case 1:
            hw[2] |= (sr << NVFX_VP(INST_SRC1_SHIFT));
            break;
        case 2:
            hw[2] |= (((sr & NVFX_VP(SRC2_HIGH_MASK)) >> NVFX_VP(SRC2_HIGH_SHIFT))
                      << NVFX_VP(INST_SRC2H_SHIFT));
            hw[3] |= ((sr & NVFX_VP(SRC2_LOW_MASK)) << NVFX_VP(INST_SRC2L_SHIFT));
            break;
        }
    }

    // --- opcode + condition code + writemask ---
    hw[0] |= (insn.cc_cond     << NVFX_VP(INST_COND_SHIFT));
    hw[0] |= (insn.cc_test     << NVFX_VP(INST_COND_TEST_SHIFT));
    hw[0] |= (insn.cc_test_reg << NVFX_VP(INST_COND_REG_SELECT_SHIFT));
    hw[0] |= ((insn.cc_swz[0] << NVFX_VP(INST_COND_SWZ_X_SHIFT)) |
              (insn.cc_swz[1] << NVFX_VP(INST_COND_SWZ_Y_SHIFT)) |
              (insn.cc_swz[2] << NVFX_VP(INST_COND_SWZ_Z_SHIFT)) |
              (insn.cc_swz[3] << NVFX_VP(INST_COND_SWZ_W_SHIFT)));
    if (insn.cc_update)     hw[0] |= NVFX_VP(INST_COND_UPDATE_ENABLE);
    if (insn.cc_update_reg) hw[0] |= NVFX_VP(INST_COND_REG_SELECT_1);
    if (insn.sat)           hw[0] |= NV40_VP_INST_SATURATE;

    if (slot == 0)
    {
        hw[1] |= (op << NV40_VP_INST_VEC_OPCODE_SHIFT);
        hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
        hw[3] |= (insn.mask << NV40_VP_INST_VEC_WRITEMASK_SHIFT);
    }
    else
    {
        hw[1] |= (op << NV40_VP_INST_SCA_OPCODE_SHIFT);
        hw[0] |= NV40_VP_INST_VEC_DEST_TEMP_MASK;
        hw[3] |= (insn.mask << NV40_VP_INST_SCA_WRITEMASK_SHIFT);
    }
}

void VpAssembler::emitCoIssued(const struct nvfx_insn& vecInsn,
                               uint8_t vecOpcode,
                               const struct nvfx_insn& scaInsn,
                               uint8_t scaOpcode)
{
    assert((static_cast<uint32_t>(vecOpcode) >> 7) == NVFX_VP_INST_SLOT_VEC);
    assert((static_cast<uint32_t>(scaOpcode) >> 7) == NVFX_VP_INST_SLOT_SCA);

    const uint32_t vecOp = static_cast<uint32_t>(vecOpcode) & 0x7f;
    const uint32_t scaOp = static_cast<uint32_t>(scaOpcode) & 0x7f;

    const size_t base = words_.size();
    words_.resize(base + 4, 0u);
    uint32_t* hw = &words_[base];

    auto markOutput = [&](int index) {
        switch (index)
        {
        case NV40_VP_INST_DEST_COL0:
        case NV40_VP_INST_DEST_COL1:
            outputMask_ |= (1u << (index - NV40_VP_INST_DEST_COL0));
            break;
        case NV40_VP_INST_DEST_BFC0:
        case NV40_VP_INST_DEST_BFC1:
            outputMask_ |= (4u << (index - NV40_VP_INST_DEST_BFC0));
            break;
        case NV40_VP_INST_DEST_FOGC: outputMask_ |= (1u << 4); break;
        case NV40_VP_INST_DEST_PSZ : outputMask_ |= (1u << 5); break;
        default:
            if (index >= NV40_VP_INST_DEST_TC(0) &&
                index <= NV40_VP_INST_DEST_TC(7))
            {
                outputMask_ |= (0x4000u << (index - NV40_VP_INST_DEST_TC0));
            }
            break;
        }
    };

    bool anyOutputWritten = false;
    auto emitDstForSlot = [&](const struct nvfx_reg& dst, uint32_t slot) {
        switch (dst.type)
        {
        case NVFXSR_NONE:
            break;

        case NVFXSR_ADDRESS:
        case NVFXSR_TEMP:
            if (numTempRegs_ < (dst.index + 1))
                numTempRegs_ = dst.index + 1;
            if (slot == NVFX_VP_INST_SLOT_VEC)
                hw[0] |= (dst.index << NV40_VP_INST_VEC_DEST_TEMP_SHIFT);
            else
                hw[3] |= (dst.index << NV40_VP_INST_SCA_DEST_TEMP_SHIFT);
            break;

        case NVFXSR_OUTPUT:
            anyOutputWritten = true;
            markOutput(dst.index);
            hw[3] |= (dst.index << NV40_VP_INST_DEST_SHIFT);
            if (slot == NVFX_VP_INST_SLOT_VEC)
            {
                hw[0] |= NV40_VP_INST_VEC_RESULT;
                hw[0] |= NV40_VP_INST_VEC_DEST_TEMP_MASK;
            }
            else
            {
                hw[3] |= NV40_VP_INST_SCA_RESULT;
                hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
            }
            break;
        }
    };

    auto sourceBits = [&](const struct nvfx_src& src) -> uint32_t {
        uint32_t sr = 0;

        switch (src.reg.type)
        {
        case NVFXSR_TEMP:
            sr |= (NVFX_VP(SRC_REG_TYPE_TEMP) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            sr |= (src.reg.index << NVFX_VP(SRC_TEMP_SRC_SHIFT));
            break;
        case NVFXSR_INPUT:
            sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            inputMask_ |= (1u << src.reg.index);
            hw[1] |= (src.reg.index << NVFX_VP(INST_INPUT_SRC_SHIFT));
            break;
        case NVFXSR_CONST:
            sr |= (NVFX_VP(SRC_REG_TYPE_CONST) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            hw[1] |= (static_cast<uint32_t>(src.reg.index) & 0x1FF)
                     << NV40_VP_INST_CONST_SRC_SHIFT;
            if (src.indirect)
            {
                hw[3] |= NV40_VP_INST_INDEX_CONST;
                if (src.indirect_reg)
                    hw[0] |= NV40_VP_INST_ADDR_REG_SELECT_1;
                hw[0] |= (static_cast<uint32_t>(src.indirect_swz) & 0x3u)
                         << NV40_VP_INST_ADDR_SWZ_SHIFT;
            }
            break;
        case NVFXSR_NONE:
            sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            break;
        default:
            sr |= (NVFX_VP(SRC_REG_TYPE_INPUT) << NVFX_VP(SRC_REG_TYPE_SHIFT));
            break;
        }

        if (src.negate) sr |= NVFX_VP(SRC_NEGATE);
        sr |= ((src.swz[0] << NVFX_VP(SRC_SWZ_X_SHIFT)) |
               (src.swz[1] << NVFX_VP(SRC_SWZ_Y_SHIFT)) |
               (src.swz[2] << NVFX_VP(SRC_SWZ_Z_SHIFT)) |
               (src.swz[3] << NVFX_VP(SRC_SWZ_W_SHIFT)));
        return sr;
    };

    auto emitSrcAt = [&](const struct nvfx_src& src, uint8_t pos) {
        const uint32_t sr = sourceBits(src);
        if (src.abs) hw[0] |= (1u << (21 + pos));

        switch (pos)
        {
        case 0:
            hw[1] |= (((sr & NVFX_VP(SRC0_HIGH_MASK)) >> NVFX_VP(SRC0_HIGH_SHIFT))
                      << NVFX_VP(INST_SRC0H_SHIFT));
            hw[2] |= ((sr & NVFX_VP(SRC0_LOW_MASK)) << NVFX_VP(INST_SRC0L_SHIFT));
            break;
        case 1:
            hw[2] |= (sr << NVFX_VP(INST_SRC1_SHIFT));
            break;
        case 2:
            hw[2] |= (((sr & NVFX_VP(SRC2_HIGH_MASK)) >> NVFX_VP(SRC2_HIGH_SHIFT))
                      << NVFX_VP(INST_SRC2H_SHIFT));
            hw[3] |= ((sr & NVFX_VP(SRC2_LOW_MASK)) << NVFX_VP(INST_SRC2L_SHIFT));
            break;
        }
    };

    // NV40 co-issue has a shared source field set.  The BasicVertex oracle
    // pairs use VEC src0/src1 and the SCA scalar source in src2.
    assert(vecInsn.src[2].reg.type == NVFXSR_NONE);
    assert(scaInsn.src[1].reg.type == NVFXSR_NONE);
    assert(scaInsn.src[2].reg.type == NVFXSR_NONE);

    emitDstForSlot(vecInsn.dst, NVFX_VP_INST_SLOT_VEC);
    emitDstForSlot(scaInsn.dst, NVFX_VP_INST_SLOT_SCA);
    if (!anyOutputWritten)
        hw[3] |= NV40_VP_INST_DEST_MASK;
    emitSrcAt(vecInsn.src[0], 0);
    emitSrcAt(vecInsn.src[1], 1);
    emitSrcAt(scaInsn.src[0], 2);

    hw[0] |= (vecInsn.cc_cond     << NVFX_VP(INST_COND_SHIFT));
    hw[0] |= (vecInsn.cc_test     << NVFX_VP(INST_COND_TEST_SHIFT));
    hw[0] |= (vecInsn.cc_test_reg << NVFX_VP(INST_COND_REG_SELECT_SHIFT));
    hw[0] |= ((vecInsn.cc_swz[0] << NVFX_VP(INST_COND_SWZ_X_SHIFT)) |
              (vecInsn.cc_swz[1] << NVFX_VP(INST_COND_SWZ_Y_SHIFT)) |
              (vecInsn.cc_swz[2] << NVFX_VP(INST_COND_SWZ_Z_SHIFT)) |
              (vecInsn.cc_swz[3] << NVFX_VP(INST_COND_SWZ_W_SHIFT)));
    if (vecInsn.cc_update)     hw[0] |= NVFX_VP(INST_COND_UPDATE_ENABLE);
    if (vecInsn.cc_update_reg) hw[0] |= NVFX_VP(INST_COND_REG_SELECT_1);
    if (vecInsn.sat)           hw[0] |= NV40_VP_INST_SATURATE;

    // None of the current oracle co-issue pairs use SCA condition flags.
    assert(!scaInsn.cc_update);
    assert(!scaInsn.cc_update_reg);
    assert(!scaInsn.cc_test);
    assert(!scaInsn.sat);

    hw[1] |= (vecOp << NV40_VP_INST_VEC_OPCODE_SHIFT);
    hw[1] |= (scaOp << NV40_VP_INST_SCA_OPCODE_SHIFT);
    hw[3] |= (vecInsn.mask << NV40_VP_INST_VEC_WRITEMASK_SHIFT);
    hw[3] |= (scaInsn.mask << NV40_VP_INST_SCA_WRITEMASK_SHIFT);
}

void VpAssembler::markLast()
{
    if (words_.empty()) return;
    words_[words_.size() - 1] |= NVFX_VP_INST_LAST;
}

}  // namespace nv40
