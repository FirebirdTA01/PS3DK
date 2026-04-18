/*
 * NV40 back-end top-level dispatch.
 *
 * Routes the IRModule to the vertex- or fragment-program emitter.  The
 * per-stage emitters live in nv40_vp_emit.cpp / nv40_fp_emit.cpp.  This
 * file exists so main.cpp only has to include one header.
 */

#include "nv40_emit.h"

#include "ir.h"

namespace nv40
{

static const IRFunction* findEntryPoint(const IRModule& module, const std::string& entry)
{
    for (const auto& fn : module.functions)
    {
        if (!fn) continue;
        if (fn->name == entry) return fn.get();
    }
    return nullptr;
}

namespace detail
{
UcodeOutput lowerVertexProgram  (const IRModule& module, const IRFunction& entry);
UcodeOutput lowerFragmentProgram(const IRModule& module, const IRFunction& entry);
}  // namespace detail

UcodeOutput emitVertexProgram(const IRModule& module, const std::string& entry)
{
    UcodeOutput out;
    const IRFunction* fn = findEntryPoint(module, entry);
    if (!fn)
    {
        out.diagnostics.push_back("nv40: entry point '" + entry + "' not found in IR module");
        return out;
    }
    return detail::lowerVertexProgram(module, *fn);
}

UcodeOutput emitFragmentProgram(const IRModule& module, const std::string& entry)
{
    UcodeOutput out;
    const IRFunction* fn = findEntryPoint(module, entry);
    if (!fn)
    {
        out.diagnostics.push_back("nv40: entry point '" + entry + "' not found in IR module");
        return out;
    }
    return detail::lowerFragmentProgram(module, *fn);
}

}  // namespace nv40
