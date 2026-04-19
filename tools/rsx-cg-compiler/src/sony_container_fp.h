#ifndef RSX_CG_COMPILER_SONY_CONTAINER_FP_H
#define RSX_CG_COMPILER_SONY_CONTAINER_FP_H

/*
 * Sony .fpo (CgBinaryProgram) emitter for fragment programs.
 *
 * Produces the byte-exact container that wraps an NV40 FP ucode blob:
 * a CgBinaryProgram header, a CgBinaryParameter table, a string region,
 * and a CgBinaryFragmentProgram subtype, each laid out per
 * cgBinary.h's documented offsets.  The header / param / subtype
 * field semantics + byte layout are recorded in
 * docs/REVERSE_ENGINEERING.md, "CgBinaryProgram header layout" and
 * "CgBinaryParameter layout" sections.
 *
 * sce-cgc cross-checked: 2026-04-18 against identity_f.cg and
 * tex_f.cg .fpo dumps from SDK 475.001's sce-cgc.exe.
 */

#include <cstdint>
#include <string>
#include <vector>

class IRModule;

namespace nv40 { struct FpAttributes; }

namespace sony
{

struct ContainerResult
{
    std::vector<uint8_t>     bytes;
    std::vector<std::string> diagnostics;
    bool                     ok = false;
};

struct ContainerOptions
{
    // Sampler names listed via `#pragma alphakill <name>` (in source-order
    // of appearance in the .cg file).  Each becomes one synthetic
    // $kill_NNNN CgBinaryParameter appended to the table.
    std::vector<std::string> alphakillSamplers;
};

ContainerResult emitFragmentContainer(
    const IRModule&             module,
    const std::string&          entryName,
    const std::vector<uint32_t>& ucode,
    const nv40::FpAttributes&   attrs,
    const ContainerOptions&     opts = {});

}  // namespace sony

#endif  /* RSX_CG_COMPILER_SONY_CONTAINER_FP_H */
