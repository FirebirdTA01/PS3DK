/*
 * NV40 back-end top-level dispatch.
 *
 * Routes the IRModule to the general lowering for its profile
 * (nv40_general_lowering.cpp).  This file exists so main.cpp only has to
 * include one header.
 */

#include "nv40_emit.h"
#include "nv40_general_lowering.h"

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

UcodeOutput emitVertexProgram(const IRModule& module, const std::string& entry,
                              const rsx_cg::CompileOptions& opts)
{
    UcodeOutput out;
    const IRFunction* fn = findEntryPoint(module, entry);
    if (!fn)
    {
        out.diagnostics.push_back("nv40: entry point '" + entry + "' not found in IR module");
        return out;
    }
    return detail::lowerVertexProgramGeneral(module, *fn, opts, nullptr);
}

VpEmitResult emitVertexProgramEx(const IRModule& module, const std::string& entry,
                                 const rsx_cg::CompileOptions& opts)
{
    VpEmitResult out;
    const IRFunction* fn = findEntryPoint(module, entry);
    if (!fn)
    {
        out.ucode.diagnostics.push_back("nv40: entry point '" + entry + "' not found in IR module");
        return out;
    }
    out.ucode = detail::lowerVertexProgramGeneral(module, *fn, opts, &out.attrs);
    return out;
}

UcodeOutput emitFragmentProgram(const IRModule& module, const std::string& entry,
                                const rsx_cg::CompileOptions& opts)
{
    UcodeOutput out;
    const IRFunction* fn = findEntryPoint(module, entry);
    if (!fn)
    {
        out.diagnostics.push_back("nv40: entry point '" + entry + "' not found in IR module");
        return out;
    }
    return detail::lowerFragmentProgramGeneral(module, *fn, opts, nullptr);
}

FpEmitResult emitFragmentProgramEx(const IRModule& module, const std::string& entry,
                                   const rsx_cg::CompileOptions& opts)
{
    FpEmitResult out;
    const IRFunction* fn = findEntryPoint(module, entry);
    if (!fn)
    {
        out.ucode.diagnostics.push_back("nv40: entry point '" + entry + "' not found in IR module");
        return out;
    }
    out.ucode = detail::lowerFragmentProgramGeneral(module, *fn, opts, &out.attrs);
    return out;
}

}  // namespace nv40
