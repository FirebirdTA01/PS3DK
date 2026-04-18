/*
 * NV40 fragment-program emitter — skeleton.
 *
 * Same contract as the vertex emitter, but produces NV40 FP ucode and
 * implements the Sony Cg pragma extensions (#pragma alphakill, #pragma
 * texformat) which NVIDIA's stock sce-cgc honours but plain NVIDIA cgc
 * silently drops.  Populated after the vertex path is proven.
 */

#include "nv40_emit.h"

#include "ir.h"

#include "nv40_vertprog.h"
#include "nv30_vertprog.h"
#include "nvfx_shader.h"

namespace nv40::detail
{

UcodeOutput lowerFragmentProgram(const IRModule& /*module*/, const IRFunction& entry)
{
    UcodeOutput out;
    out.diagnostics.push_back(
        "nv40-fp: lowering not yet implemented (entry=" + entry.name + ")");
    out.ok = false;
    return out;
}

}  // namespace nv40::detail
