#ifndef RSX_CG_COMPILER_NV40_EMIT_H
#define RSX_CG_COMPILER_NV40_EMIT_H

/*
 * Public entry points for the NV40 back-end.
 *
 * Each emit routine takes the hardware-agnostic IRModule produced by
 * the donor front-end and returns raw ucode words plus diagnostics.
 * The container format (.fpo/.vpo — CellCgbVertexProgramConfiguration
 * / CellCgbFragmentProgramConfiguration) is produced by a separate
 * sony_container_* layer once emit is stable.
 */

#include <cstdint>
#include <string>
#include <vector>

class IRModule;

namespace nv40
{

struct UcodeOutput
{
    std::vector<uint32_t>    words;
    std::vector<std::string> diagnostics;
    bool                     ok = false;

    bool hasOutput() const { return ok && !words.empty(); }
};

// Fields needed to populate the CgBinaryFragmentProgram subtype of a
// .fpo container.  Filled by lowerFragmentProgram alongside ucode
// emission and surfaced via emitFragmentProgramEx.
struct FpAttributes
{
    uint32_t attributeInputMask  = 0;        // bits per FP varying read
    uint32_t partialTexType      = 0;        // 2 bits per texunit; 0 = full load
    uint16_t texCoordsInputMask  = 0;        // bit n = TEXCOORDn used
    uint16_t texCoords2D         = 0xFFFFu;  // bit n = TEXCOORDn is 2D (sce-cgc default: all 1)
    uint16_t texCoordsCentroid   = 0;
    uint8_t  registerCount       = 2;        // sce-cgc minimum
    uint8_t  outputFromH0        = 0;        // 1 iff R0 is fp16 (H0)
    uint8_t  depthReplace        = 0;        // 1 iff DEPTH output written
    uint8_t  pixelKill           = 0;        // 1 iff KIL opcode emitted
};

struct FpEmitResult
{
    UcodeOutput  ucode;
    FpAttributes attrs;
};

// Fields needed to populate the CgBinaryVertexProgram subtype of a
// .vpo container.  Filled by lowerVertexProgram alongside ucode
// emission and surfaced via emitVertexProgramEx.
struct VpAttributes
{
    uint32_t instructionSlot     = 0;        // load address; non-zero enables indexed reads
    uint32_t registerCount       = 1;        // R registers used; sce-cgc minimum is 1
    uint32_t attributeInputMask  = 0;        // bit n iff v[n] is read
    uint32_t attributeOutputMask = 0;        // SET_VERTEX_ATTRIB_OUTPUT_MASK bits, front-face only
    uint32_t userClipMask        = 0;        // user clip plane enables
};

struct VpEmitResult
{
    UcodeOutput  ucode;
    VpAttributes attrs;
};

UcodeOutput  emitVertexProgram    (const IRModule& module, const std::string& entry);
UcodeOutput  emitFragmentProgram  (const IRModule& module, const std::string& entry);
FpEmitResult emitFragmentProgramEx(const IRModule& module, const std::string& entry);
VpEmitResult emitVertexProgramEx  (const IRModule& module, const std::string& entry);

}  // namespace nv40

#endif  /* RSX_CG_COMPILER_NV40_EMIT_H */
