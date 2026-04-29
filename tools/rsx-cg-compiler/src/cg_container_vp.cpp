/*
 * .vpo (CgBinaryProgram + CgBinaryVertexProgram) emitter.
 *
 * Layout mirrors cg_container_fp.cpp; see that file for the section
 * map and alignment rules.  Only the differences are noted here:
 *
 *   - Subtype `CgBinaryVertexProgram` (cgBinary.h): 6 u32 fields,
 *     24 bytes content.  Padded to 16-byte alignment before ucode.
 *   - VP semantic→resource mapping (CGresource enum):
 *
 *     VP input semantics map to vertex attributes (ATTR0..15):
 *       POSITION    → CG_ATTR0  = 0x841   (NVIDIA's auto-bind table)
 *       BLENDWEIGHT → CG_ATTR1  = 0x842
 *       NORMAL      → CG_ATTR2  = 0x843
 *       COLOR       → CG_ATTR3  = 0x844
 *       COLOR1      → CG_ATTR4  = 0x845
 *       TEXCOORDn   → CG_ATTR(8+n) = 0x848+n
 *
 *     VP output semantics use the post-rasteriser output bank:
 *       POSITION  / HPOS      → CG_HPOS    = 0x8c3
 *       COLOR     / COL0      → CG_COL0    = 0x8c5
 *       COLOR1    / COL1      → CG_COL1    = 0x8c6
 *       TEXCOORDn / TEXn      → CG_TEX0+n  = 0x883+n  (note: VP's
 *                                                       TEX0 vs FP's
 *                                                       TEXCOORD0)
 *
 *     Uniforms land in the const bank (C[0..511]):
 *       float4x4   → CG_C = 0x882, resIndex = first row's c[N]
 *       float4     → CG_C = 0x882, resIndex = scalar's c[N]
 *       sampler*   → not handled here yet (no VP sampler shader)
 *
 *   - Matrix uniforms expand: one "parent" CgBinaryParameter with
 *     type=CG_FLOAT4x4 + N child rows with type=CG_FLOAT4 sharing
 *     the same paramno.
 *
 * Bails out when entry parameters look struct-flattened (single
 * `void` param + ldattr/stout in the IR) — the reference compiler
 * names struct fields as `<param>.<field>` and `<func>.<field>`,
 * which our front-end doesn't yet preserve.  The diff harness falls
 * back to ucode-only diff for those shaders.
 */

#include "cg_container_vp.h"
#include "nv40/nv40_emit.h"

#include "ir.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <unordered_map>

namespace cg_container
{

namespace
{

constexpr uint32_t kProfileVpRsx           = 0x00001b5bu;
constexpr uint32_t kBinaryFormatRevision   = 6u;
constexpr uint32_t kCgIn                   = 0x00001001u;
constexpr uint32_t kCgOut                  = 0x00001002u;
constexpr uint32_t kCgVarying              = 0x00001005u;
constexpr uint32_t kCgUniform              = 0x00001006u;
// Literal-pool param marker (sce-cgc emits this for `internal-constant-N`):
//   - var field = 0x1007 (a third class beside Varying/Uniform)
//   - paramno   = 0xFFFFFFFE (~kInvalidIndex - 1, meaning "synthesised
//                              by the compiler, not a user parameter")
//   - defaultValue field of the param entry points at a 16-byte
//     float[4] block embedded between the user-visible strings and
//     the param's own name string.
constexpr uint32_t kCgInternalConst        = 0x00001007u;
constexpr uint32_t kInvalidIndex           = 0xFFFFFFFFu;
constexpr uint32_t kLiteralPoolParamno     = 0xFFFFFFFEu;

// CGresource codes (from cg_bindlocations.h).
constexpr uint32_t kCgAttr0       = 2113u;  // 0x0841 — VP POSITION input
constexpr uint32_t kCgAttr3       = 2116u;  // 0x0844 — VP COLOR input
constexpr uint32_t kCgAttr8       = 2121u;  // 0x0848 — VP TEXCOORD0 input
constexpr uint32_t kCgConst       = 2178u;  // 0x0882 — uniform const-bank slot
constexpr uint32_t kCgTex0        = 2179u;  // 0x0883 — VP TEX/TEXCOORD output
constexpr uint32_t kCgHpos        = 2243u;  // 0x08c3 — VP POSITION output
constexpr uint32_t kCgCol0        = 2245u;  // 0x08c5 — VP COL0 output

// CGtype values (from cg_datatypes.h, base = 1024).
constexpr uint32_t kCgFloat       = 1045u;
constexpr uint32_t kCgFloat2      = 1046u;
constexpr uint32_t kCgFloat3      = 1047u;
constexpr uint32_t kCgFloat4      = 1048u;
constexpr uint32_t kCgFloat4x4    = 1064u;

std::string toUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

inline uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

void put32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void putString(std::vector<uint8_t>& out, const std::string& s)
{
    out.insert(out.end(), s.begin(), s.end());
    out.push_back('\0');
}

void padTo(std::vector<uint8_t>& out, size_t alignment)
{
    while (out.size() % alignment) out.push_back(0);
}

uint32_t cgTypeForIRType(const IRTypeInfo& t)
{
    if (t.isMatrix() && t.matrixRows == 4 && t.matrixCols == 4)
        return kCgFloat4x4;
    switch (t.baseType)
    {
    case IRType::Float32: return kCgFloat;
    case IRType::Vec2:    return kCgFloat2;
    case IRType::Vec3:    return kCgFloat3;
    case IRType::Vec4:    return kCgFloat4;
    default:              return 0;
    }
}

// VP input semantic → CGresource.  POSITION and COLOR get fixed
// vertex-attribute slots per NVIDIA's auto-bind table.
uint32_t vpInputResource(const std::string& semUpper, int semIndex)
{
    if (semUpper == "POSITION") return kCgAttr0;
    if (semUpper == "NORMAL")   return kCgAttr0 + 2;        // ATTR2
    if (semUpper == "COLOR" || semUpper == "COL")
        return kCgAttr3 + (semIndex == 1 ? 1 : 0);          // ATTR3 / ATTR4
    if (semUpper == "BLENDWEIGHT" || semUpper == "WEIGHT")
        return kCgAttr0 + 1;                                 // ATTR1
    if (semUpper == "TEXCOORD" || semUpper == "TEX")
        return kCgAttr8 + semIndex;                          // ATTR8..15
    return 0;
}

// CG_TEXCOORD0..7 — VP output binding code emitted when the source
// uses the `TEXCOORD<N>` spelling (3220+N).  Distinct from CG_TEX0
// (2179+N), which the reference compiler picks when the source spells
// the semantic as `TEX<N>`.  We honour the source spelling so the
// container is byte-exact against the reference compiler.
constexpr uint32_t kCgTexCoord0 = 3220u;  // 0x0c94

// VP output semantic → CGresource.  `rawSpelling` carries the user's
// original lexeme (e.g. "TEXCOORD0" vs "TEX0") so we can pick between
// CG_TEXCOORD<N> (0x0c94) and CG_TEX<N> (0x0883) per the reference
// compiler's encoding table.  `semUpper` is the digit-stripped form.
uint32_t vpOutputResource(const std::string& semUpper, int semIndex,
                          const std::string& rawSpelling = std::string{})
{
    if (semUpper == "POSITION" || semUpper == "HPOS")
        return kCgHpos;
    if (semUpper == "COLOR" || semUpper == "COL")
        return kCgCol0 + (semIndex == 1 ? 1 : 0);
    if (semUpper == "TEXCOORD" || semUpper == "TEX")
    {
        // Source spelling decides the bank: "TEXCOORD<N>" → CG_TEXCOORD0,
        // "TEX<N>" → CG_TEX0.  Default to CG_TEX0 if the spelling is
        // ambiguous (e.g. the IR didn't carry a raw name through).
        const bool wroteTexCoord =
            rawSpelling.size() >= 8 &&
            (rawSpelling[0] == 'T' || rawSpelling[0] == 't') &&
            std::equal(rawSpelling.begin(), rawSpelling.begin() + 8,
                       "TEXCOORD",
                       [](char a, char b) {
                           return std::toupper(static_cast<unsigned char>(a))
                                  == static_cast<unsigned char>(b);
                       });
        return (wroteTexCoord ? kCgTexCoord0 : kCgTex0) + semIndex;
    }
    return 0;
}

}  // namespace

VpContainerResult emitVertexContainer(
    const IRModule&              module,
    const std::string&           entryName,
    const std::vector<uint32_t>& ucode,
    const nv40::VpAttributes&    attrs)
{
    VpContainerResult result;

    const IRFunction* entry = nullptr;
    for (const auto& fn : module.functions)
    {
        if (fn && fn->name == entryName) { entry = fn.get(); break; }
    }
    if (!entry)
    {
        result.diagnostics.push_back(
            "cg-container-vp: entry '" + entryName + "' not in IR module");
        return result;
    }

    // Detect struct-flattened entry params.  If any IR parameter is
    // typed `void`, the IR builder collapsed an entry-point struct
    // into a single placeholder + per-field LdAttr/StOut.  In that
    // case we synthesise the parameter table from the LdAttr/StOut
    // walk instead of from entry->parameters.
    //
    // sce-cgc names struct fields as:
    //   - inputs:  `<param-instance-name>.<field-name>`  ("input.pos")
    //   - outputs: `<entry-function-name>.<field-name>`  ("main.pos")
    //
    // Both pieces are now carried on each LoadAttribute /
    // StoreOutput instruction by the IR builder; we synthesise
    // ParamDescs from them here.
    const bool isStructFlattened =
        std::any_of(entry->parameters.begin(), entry->parameters.end(),
                    [](const IRParameter& p) {
                        return p.type.baseType == IRType::Void;
                    });

    // ----- Build the parameter list, in the same iteration order as
    // sce-cgc: shader entry parameters in declaration order, with
    // matrix uniforms expanded into parent + N row entries. -----
    struct ParamDesc
    {
        std::string name;
        std::string semantic;
        uint32_t    type;
        uint32_t    res;
        uint32_t    var;
        uint32_t    direction;
        uint32_t    paramno;
        uint32_t    resIndex    = kInvalidIndex;
        uint32_t    isReferenced = 1;
        uint32_t    isShared     = 0;
        // Literal-pool slot — non-empty only for `internal-constant-N`
        // params.  Holds the four packed float values; the param's
        // `defaultValue` table field will be backfilled with the
        // offset of the embedded 16-byte block during strings emit.
        bool        isLiteralPool = false;
        float       litValues[4]   = {0, 0, 0, 0};
        uint32_t    defaultValueOffset = 0;
    };

    std::vector<ParamDesc> params;
    params.reserve(entry->parameters.size() + 4);

    // Mirror VP allocator: matrices grow from c[256] upward, scalars
    // from c[467] downward.  See REVERSE_ENGINEERING.md "Const-register
    // allocation" — same algorithm runs inside lowerVertexProgram.
    int nextMatrixReg = 256;
    int nextVectorReg = 467;

    // ----- Struct-flattened path: synthesize params from
    // LdAttr / StOut walk.  Per sce-cgc:
    //   - all input struct fields share the same paramno = 0 (they
    //     all come from the single struct entry param)
    //   - output struct fields have paramno = 0xFFFFFFFF — they're
    //     synthesised from `return o;`, not from a user param
    if (isStructFlattened)
    {
        // Find the user paramno for the struct entry parameter
        // (typically 0 — it's the only param in struct-style entries).
        uint32_t inputStructParamNo = 0;
        for (size_t i = 0; i < entry->parameters.size(); ++i)
        {
            if (entry->parameters[i].type.baseType == IRType::Void)
            {
                inputStructParamNo = static_cast<uint32_t>(i);
                break;
            }
        }

        for (const auto& blockPtr : entry->blocks)
        {
            if (!blockPtr) continue;
            for (const auto& instPtr : blockPtr->instructions)
            {
                if (!instPtr) continue;
                const IRInstruction& in = *instPtr;
                if (in.op == IROp::LoadAttribute)
                {
                    ParamDesc d;
                    d.name      = (!in.structParamName.empty() ? in.structParamName + "." : std::string{})
                                + in.fieldName;
                    d.semantic  = in.rawSemanticName.empty() ? in.semanticName : in.rawSemanticName;
                    d.type      = cgTypeForIRType(in.resultType);
                    d.var       = kCgVarying;
                    d.direction = kCgIn;
                    d.paramno   = inputStructParamNo;
                    d.res       = vpInputResource(toUpper(in.semanticName), in.semanticIndex);
                    params.push_back(d);
                }
            }
        }
        // File-scope uniforms — sce-cgc emits these between the
        // struct-flat inputs and outputs.  They're treated as
        // "shared" (constant-buffer linkage rather than function
        // parameters): paramno = 0xFFFFFFFF (synthetic) and isShared
        // = 1.  Matrices expand into a parent float4x4 entry plus N
        // float4 row entries that share the parent's semantic
        // string.  An explicit `: register(CN)` binding pins
        // resIndex to the requested const slot; otherwise the auto-
        // allocator picks one (matrices grow up from c[256]).
        for (const auto& g : module.globals)
        {
            if (g.storage != StorageQualifier::Uniform) continue;

            const bool hasExplicit = (g.explicitRegisterBank == 'C');
            std::string semantic;
            if (hasExplicit)
            {
                semantic = "C" + std::to_string(g.explicitRegisterIndex);
            }

            ParamDesc d;
            d.name      = g.name;
            d.semantic  = semantic;
            d.type      = cgTypeForIRType(g.type);
            d.var       = kCgUniform;
            d.direction = kCgIn;
            d.res       = kCgConst;
            d.paramno   = kInvalidIndex;
            d.isReferenced = 1;
            d.isShared     = 1;

            if (g.type.isMatrix())
            {
                int base;
                if (hasExplicit)
                {
                    base = g.explicitRegisterIndex;
                }
                else
                {
                    base = nextMatrixReg;
                    nextMatrixReg += g.type.matrixRows;
                }
                d.resIndex = static_cast<uint32_t>(base);
                params.push_back(d);
                for (int row = 0; row < g.type.matrixRows; ++row)
                {
                    ParamDesc r;
                    r.name      = g.name + "[" + std::to_string(row) + "]";
                    r.semantic  = semantic;
                    r.type      = kCgFloat4;
                    r.var       = kCgUniform;
                    r.direction = kCgIn;
                    r.res       = kCgConst;
                    r.paramno   = kInvalidIndex;
                    r.isReferenced = 1;
                    r.isShared     = 1;
                    r.resIndex     = static_cast<uint32_t>(base + row);
                    params.push_back(r);
                }
            }
            else
            {
                int reg;
                if (hasExplicit)
                {
                    reg = g.explicitRegisterIndex;
                }
                else
                {
                    reg = nextVectorReg--;
                }
                d.resIndex = static_cast<uint32_t>(reg);
                params.push_back(d);
            }
        }

        for (const auto& blockPtr : entry->blocks)
        {
            if (!blockPtr) continue;
            for (const auto& instPtr : blockPtr->instructions)
            {
                if (!instPtr) continue;
                const IRInstruction& in = *instPtr;
                if (in.op == IROp::StoreOutput)
                {
                    if (in.fieldName.empty()) continue;  // non-struct out; not in scope here
                    ParamDesc d;
                    d.name      = entry->name + "." + in.fieldName;   // sce-cgc convention
                    d.semantic  = in.rawSemanticName.empty() ? in.semanticName : in.rawSemanticName;
                    // Output struct fields are always vec4 in the
                    // shaders we've seen — refine when narrower-output
                    // shaders land.
                    d.type      = kCgFloat4;
                    d.var       = kCgVarying;
                    d.direction = kCgOut;
                    d.paramno   = kInvalidIndex;   // synthetic — no user param number
                    d.res       = vpOutputResource(toUpper(in.semanticName),
                                                   in.semanticIndex,
                                                   in.rawSemanticName);
                    params.push_back(d);
                }
            }
        }
    }
    else
    for (size_t i = 0; i < entry->parameters.size(); ++i)
    {
        const auto& p = entry->parameters[i];
        ParamDesc d;
        d.name      = p.name;
        d.semantic  = p.rawSemanticName.empty() ? p.semanticName : p.rawSemanticName;
        d.type      = cgTypeForIRType(p.type);
        d.paramno   = static_cast<uint32_t>(i);

        const bool isUniform = (p.storage == StorageQualifier::Uniform);
        if (isUniform)
        {
            d.var       = kCgUniform;
            d.direction = kCgIn;
            d.res       = kCgConst;
            if (p.type.isMatrix())
            {
                const int base = nextMatrixReg;
                nextMatrixReg += p.type.matrixRows;
                d.resIndex = static_cast<uint32_t>(base);
                params.push_back(d);
                // Expand into per-row entries.  Each child shares the
                // parent's paramno (sce-cgc keeps the user-facing
                // parameter index for all rows).
                for (int row = 0; row < p.type.matrixRows; ++row)
                {
                    ParamDesc r;
                    r.name      = p.name + "[" + std::to_string(row) + "]";
                    r.semantic  = "";
                    r.type      = kCgFloat4;
                    r.res       = kCgConst;
                    r.var       = kCgUniform;
                    r.direction = kCgIn;
                    r.paramno   = static_cast<uint32_t>(i);
                    r.resIndex  = static_cast<uint32_t>(base + row);
                    params.push_back(r);
                }
                continue;
            }
            else
            {
                const int reg = nextVectorReg--;
                d.resIndex = static_cast<uint32_t>(reg);
            }
        }
        else
        {
            d.var = kCgVarying;
            const bool isOut = (p.storage == StorageQualifier::Out);
            d.direction = isOut ? kCgOut : kCgIn;
            const std::string semUpper = toUpper(p.semanticName);
            d.res = isOut ? vpOutputResource(semUpper, p.semanticIndex,
                                             p.rawSemanticName)
                          : vpInputResource (semUpper, p.semanticIndex);
        }
        params.push_back(d);
    }

    // ----- Append literal-pool params (one per c[N] reg the back-end
    // reserved for unique float literals).  These follow user-declared
    // params in the param table and get their own embedded 16-byte
    // default-value blocks in the strings region. -----
    for (size_t i = 0; i < attrs.literalPool.size(); ++i)
    {
        const auto& slot = attrs.literalPool[i];
        ParamDesc d;
        d.name      = "internal-constant-" + std::to_string(i);
        d.semantic  = "";
        d.type      = (slot.usedLanes == 1) ? kCgFloat
                    : (slot.usedLanes == 2) ? kCgFloat2
                    : (slot.usedLanes == 3) ? kCgFloat3
                    :                         kCgFloat4;
        d.res       = kCgConst;
        d.var       = kCgInternalConst;
        d.direction = kCgIn;
        d.paramno   = kLiteralPoolParamno;
        d.resIndex  = slot.constReg;
        d.isReferenced = 1;
        d.isShared     = 0;
        d.isLiteralPool = true;
        d.litValues[0] = slot.values[0];
        d.litValues[1] = slot.values[1];
        d.litValues[2] = slot.values[2];
        d.litValues[3] = slot.values[3];
        params.push_back(d);
    }

    // ----- Strings region: per-param, semantic\0 then name\0.
    // Literal-pool params interleave a 16-byte float[4] block between
    // the prior strings and the param's own name string (sce-cgc lays
    // it out exactly like that — defaultValue points at the floats,
    // nameOffset points at the trailing name). -----
    const uint32_t headerSize     = 0x20u;
    const uint32_t paramTableSize = static_cast<uint32_t>(params.size() * 48u);
    const uint32_t stringsStart   = headerSize + paramTableSize;

    std::vector<uint8_t> stringsBlob;
    struct StringSlots { uint32_t semanticOffset = 0; uint32_t nameOffset = 0; };
    std::vector<StringSlots> slots(params.size());

    auto padBlobTo = [&](size_t alignment)
    {
        while (stringsBlob.size() % alignment) stringsBlob.push_back(0);
    };

    for (size_t i = 0; i < params.size(); ++i)
    {
        if (params[i].isLiteralPool)
        {
            // Pad to 16-byte alignment, emit float[4] block, point
            // defaultValueOffset at it.  The 16-byte alignment is what
            // sce-cgc uses; without it the runtime's load step picks
            // up garbage from the prior string's tail.
            padBlobTo(16);
            params[i].defaultValueOffset =
                stringsStart + static_cast<uint32_t>(stringsBlob.size());
            for (int j = 0; j < 4; ++j)
            {
                uint32_t bits;
                std::memcpy(&bits, &params[i].litValues[j], sizeof(bits));
                stringsBlob.push_back(static_cast<uint8_t>((bits >> 24) & 0xFF));
                stringsBlob.push_back(static_cast<uint8_t>((bits >> 16) & 0xFF));
                stringsBlob.push_back(static_cast<uint8_t>((bits >>  8) & 0xFF));
                stringsBlob.push_back(static_cast<uint8_t>((bits >>  0) & 0xFF));
            }
        }
        if (!params[i].semantic.empty())
        {
            slots[i].semanticOffset =
                stringsStart + static_cast<uint32_t>(stringsBlob.size());
            putString(stringsBlob, params[i].semantic);
        }
        if (!params[i].name.empty())
        {
            slots[i].nameOffset =
                stringsStart + static_cast<uint32_t>(stringsBlob.size());
            putString(stringsBlob, params[i].name);
        }
    }

    const uint32_t stringsEndUnpadded =
        stringsStart + static_cast<uint32_t>(stringsBlob.size());
    const uint32_t programOffset = align16(stringsEndUnpadded);

    // CgBinaryVertexProgram subtype = 24 bytes content; pad to 16.
    constexpr uint32_t programSubtypeBytes = 24u;
    const uint32_t ucodeOffset =
        align16(programOffset + programSubtypeBytes);

    const uint32_t ucodeSize = static_cast<uint32_t>(ucode.size() * 4u);
    const uint32_t totalSize = ucodeOffset + ucodeSize;

    auto& out = result.bytes;
    out.reserve(totalSize);

    // Header.
    put32(out, kProfileVpRsx);
    put32(out, kBinaryFormatRevision);
    put32(out, totalSize);
    put32(out, static_cast<uint32_t>(params.size()));
    put32(out, headerSize);
    put32(out, programOffset);
    put32(out, ucodeSize);
    put32(out, ucodeOffset);

    // Parameter table.
    for (size_t i = 0; i < params.size(); ++i)
    {
        const auto& d = params[i];
        put32(out, d.type);
        put32(out, d.res);
        put32(out, d.var);
        put32(out, d.resIndex);
        put32(out, slots[i].nameOffset);
        put32(out, d.defaultValueOffset);    // 0 unless this is a literal-pool entry
        put32(out, 0);                       // embeddedConst
        put32(out, slots[i].semanticOffset);
        put32(out, d.direction);
        put32(out, d.paramno);
        put32(out, d.isReferenced);
        put32(out, d.isShared);
    }

    // Strings + padding.
    out.insert(out.end(), stringsBlob.begin(), stringsBlob.end());
    padTo(out, 16);

    // CgBinaryVertexProgram subtype.
    put32(out, static_cast<uint32_t>(ucode.size() / 4));     // instructionCount
    put32(out, attrs.instructionSlot);
    put32(out, attrs.registerCount);
    put32(out, attrs.attributeInputMask);
    put32(out, attrs.attributeOutputMask);
    put32(out, attrs.userClipMask);
    padTo(out, 16);

    // ucode (big-endian raw words).
    for (uint32_t w : ucode) put32(out, w);

    if (out.size() != totalSize)
    {
        result.diagnostics.push_back(
            "cg-container-vp: emitted " + std::to_string(out.size()) +
            " bytes, expected " + std::to_string(totalSize));
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace cg_container
