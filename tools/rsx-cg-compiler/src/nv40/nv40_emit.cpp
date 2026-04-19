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
UcodeOutput lowerVertexProgram  (const IRModule& module, const IRFunction& entry,
                                 const rsx_cg::CompileOptions& opts,
                                 VpAttributes* attrsOut);
UcodeOutput lowerFragmentProgram(const IRModule& module, const IRFunction& entry,
                                 const rsx_cg::CompileOptions& opts,
                                 FpAttributes* attrsOut);
}  // namespace detail

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
    return detail::lowerVertexProgram(module, *fn, opts, nullptr);
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
    out.ucode = detail::lowerVertexProgram(module, *fn, opts, &out.attrs);
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
    return detail::lowerFragmentProgram(module, *fn, opts, nullptr);
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
    out.ucode = detail::lowerFragmentProgram(module, *fn, opts, &out.attrs);
    return out;
}

}  // namespace nv40
