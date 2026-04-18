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

UcodeOutput emitVertexProgram  (const IRModule& module, const std::string& entry);
UcodeOutput emitFragmentProgram(const IRModule& module, const std::string& entry);

}  // namespace nv40

#endif  /* RSX_CG_COMPILER_NV40_EMIT_H */
