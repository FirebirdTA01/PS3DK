#ifndef RSX_CG_COMPILER_NV40_GENERAL_LOWERING_H
#define RSX_CG_COMPILER_NV40_GENERAL_LOWERING_H

#include "compile_options.h"
#include "nv40_emit.h"

class IRFunction;
class IRModule;

namespace nv40::detail
{

UcodeOutput lowerFragmentProgramGeneral(const IRModule& module,
                                        const IRFunction& entry,
                                        const rsx_cg::CompileOptions& opts,
                                        FpAttributes* attrsOut);

UcodeOutput lowerVertexProgramGeneral(const IRModule& module,
                                      const IRFunction& entry,
                                      const rsx_cg::CompileOptions& opts,
                                      VpAttributes* attrsOut);

}  // namespace nv40::detail

#endif  /* RSX_CG_COMPILER_NV40_GENERAL_LOWERING_H */
