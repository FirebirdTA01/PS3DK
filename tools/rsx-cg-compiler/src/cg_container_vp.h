#ifndef RSX_CG_COMPILER_CG_CONTAINER_VP_H
#define RSX_CG_COMPILER_CG_CONTAINER_VP_H

/*
 * .vpo (CgBinaryProgram) emitter for vertex programs.
 *
 * Same pattern as the FP container: header + parameter table +
 * strings + CgBinaryVertexProgram subtype + ucode, all big-endian
 * with 16-byte aligned sections.  Differences from FP:
 *
 *   - Profile = sce_vp_rsx (0x1b5b vs FP's 0x1b5c).
 *   - Resource codes: VP `: POSITION` → CG_ATTR0 (0x841), VP
 *     `: COLOR` (input) → CG_ATTR3 (0x844), VP TEXCOORD<n> input
 *     → CG_ATTR8+n (0x848+n).  Outputs use HPOS (0x8c3) /
 *     COL0 (0x8c5) / TEX<n> (0xc94+n — same as FP TEXCOORD<n>).
 *     Uniforms use CG_C (0x882) with resIndex = const-bank slot.
 *   - Matrix uniforms expand to one parent CgBinaryParameter +
 *     N row entries (paramno shared, resIndex differs by row).
 *   - Subtype is CgBinaryVertexProgram (24 bytes content): six
 *     u32 fields — instructionCount, instructionSlot, registerCount,
 *     attributeInputMask, attributeOutputMask, userClipMask.
 *
 * Reference-compiler cross-checked against const_out_v.vpo,
 * add_uniforms_v.vpo, mvp_passthrough_v.vpo, and probe_tc_v.vpo.
 */

#include <cstdint>
#include <string>
#include <vector>

class IRModule;

namespace nv40 { struct VpAttributes; }

namespace cg_container
{

struct VpContainerResult
{
    std::vector<uint8_t>     bytes;
    std::vector<std::string> diagnostics;
    bool                     ok = false;
};

VpContainerResult emitVertexContainer(
    const IRModule&              module,
    const std::string&           entryName,
    const std::vector<uint32_t>& ucode,
    const nv40::VpAttributes&    attrs);

}  // namespace cg_container

#endif  /* RSX_CG_COMPILER_CG_CONTAINER_VP_H */
