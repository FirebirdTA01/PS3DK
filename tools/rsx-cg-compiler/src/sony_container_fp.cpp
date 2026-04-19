/*
 * Sony .fpo (CgBinaryProgram + CgBinaryFragmentProgram) emitter.
 *
 * Layout (offsets in the resulting blob, big-endian throughout):
 *
 *   0x00  CgBinaryProgram header (32 bytes — see cgBinary.h)
 *   0x20  CgBinaryParameter[parameterCount] (48 bytes each)
 *         (immediately after the header — sce-cgc lays this out
 *          contiguously without any pad, parameterArray = 0x20)
 *   <X>   string region (zero-terminated names + semantics, in
 *         per-parameter declaration order: emit semantic, then name).
 *         Padded with NUL bytes so the next section starts at a
 *         16-byte boundary.
 *   <Y>   CgBinaryFragmentProgram subtype (22 bytes content +
 *         padding to a 16-byte boundary).
 *   <Z>   raw ucode (16-byte aligned).
 *
 * Strings are NOT deduplicated — sce-cgc emits a fresh copy of each
 * semantic + name per parameter, even when two parameters share the
 * same semantic ("COLOR" appears twice in identity_f.fpo).
 *
 * Resource codes (CGresource enum, from cg_bindlocations.h):
 *   - FP COLOR / COLOR0 → CG_COLOR0  = 2757 = 0x0ac5
 *   - FP TEXCOORDn      → CG_TEXCOORD0 + n = 3220+n = 0x0c94+n
 *   - FP sampler        → CG_TEXUNIT0 + n  = 2048+n = 0x0800+n
 *
 * CGtype values (from cg_datatypes.h, indexed from CG_TYPE_START_ENUM=1024):
 *   - CG_FLOAT2    = 1046 = 0x0416
 *   - CG_FLOAT3    = 1047 = 0x0417
 *   - CG_FLOAT4    = 1048 = 0x0418
 *   - CG_FLOAT4x4  = 1064 = 0x0428
 *   - CG_SAMPLER2D = 1066 = 0x042a
 *
 * CGenum (from cg_enums.h):
 *   - CG_IN      = 4097 = 0x1001
 *   - CG_OUT     = 4098 = 0x1002
 *   - CG_VARYING = 4101 = 0x1005
 *   - CG_UNIFORM = 4102 = 0x1006
 */

#include "sony_container_fp.h"
#include "nv40/nv40_emit.h"

#include "ir.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace sony
{

namespace
{

constexpr uint32_t kProfileFpRsx           = 0x00001b5cu;
constexpr uint32_t kBinaryFormatRevision   = 6u;
constexpr uint32_t kCgIn                   = 0x00001001u;
constexpr uint32_t kCgOut                  = 0x00001002u;
constexpr uint32_t kCgInOut                = 0x00001003u;
constexpr uint32_t kCgVarying              = 0x00001005u;
constexpr uint32_t kCgUniform              = 0x00001006u;
constexpr uint32_t kInvalidIndex           = 0xFFFFFFFFu;

// CGresource bind locations.
constexpr uint32_t kCgTexUnit0   = 2048u;  // 0x0800
constexpr uint32_t kCgColor0     = 2757u;  // 0x0ac5  (FP COLOR semantic)
constexpr uint32_t kCgTexCoord0  = 3220u;  // 0x0c94

// Sce-cgc's "kill marker" resource code — used on the synthetic
// $kill_NNNN parameter that #pragma alphakill <samplerName> produces.
// Not in the documented CG_BINDLOCATION_MACRO table; reverse-engineered
// from sce-cgc output (see REVERSE_ENGINEERING.md "alphakill" section).
constexpr uint32_t kCgKillMarker = 0x0cb8u;

// CGtype values.
constexpr uint32_t kCgFloat       = 1045u;
constexpr uint32_t kCgFloat2      = 1046u;
constexpr uint32_t kCgFloat3      = 1047u;
constexpr uint32_t kCgFloat4      = 1048u;
constexpr uint32_t kCgFloat4x4    = 1064u;
constexpr uint32_t kCgSampler2D   = 1066u;
constexpr uint32_t kCgSamplerCube = 1069u;

std::string toUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

// Round a byte offset up to a 16-byte boundary.
inline uint32_t align16(uint32_t v) { return (v + 15u) & ~15u; }

// Big-endian writers.
void put32(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put16(std::vector<uint8_t>& out, uint16_t v)
{
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

// Map an IR type to a CGtype enum value.  Only the subset we currently
// support is mapped; the rest will be filled in as more shaders need
// them.
uint32_t cgTypeForIRType(const IRTypeInfo& t)
{
    if (t.baseType == IRType::Sampler2D)   return kCgSampler2D;
    if (t.baseType == IRType::SamplerCube) return kCgSamplerCube;
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

// FP semantic → CGresource code.  Returns 0 when the semantic doesn't
// map (uniform parameters, etc.).
uint32_t fpResourceFor(const std::string& semUpper, int semIndex)
{
    if (semUpper == "COLOR" || semUpper == "COL")
        return kCgColor0 + (semIndex == 1 ? 1 : 0);
    if (semUpper == "TEXCOORD" || semUpper == "TEX")
        return kCgTexCoord0 + semIndex;
    return 0;
}

}  // namespace

ContainerResult emitFragmentContainer(
    const IRModule&              module,
    const std::string&           entryName,
    const std::vector<uint32_t>& ucode,
    const nv40::FpAttributes&    attrs,
    const ContainerOptions&      opts)
{
    ContainerResult result;

    // Find entry function.
    const IRFunction* entry = nullptr;
    for (const auto& fn : module.functions)
    {
        if (fn && fn->name == entryName) { entry = fn.get(); break; }
    }
    if (!entry)
    {
        result.diagnostics.push_back(
            "sony-fp: entry '" + entryName + "' not in IR module");
        return result;
    }

    // ----- Parameter table description.  Same iteration order as
    // sce-cgc: shader entry parameters, in declaration order. -----
    struct ParamDesc
    {
        std::string name;
        std::string semantic;
        uint32_t    type;
        uint32_t    res;
        uint32_t    var;
        uint32_t    direction;
        uint32_t    paramno;
        uint32_t    isReferenced = 1;
    };

    std::vector<ParamDesc> params;
    params.reserve(entry->parameters.size());

    int nextSamplerUnit = 0;
    for (size_t i = 0; i < entry->parameters.size(); ++i)
    {
        const auto& p = entry->parameters[i];
        ParamDesc d;
        d.name      = p.name;
        // Preserve original source spelling — sce-cgc stores "TEXCOORD0"
        // when the user wrote "TEXCOORD0", and "TEXCOORD" when they wrote
        // "TEXCOORD".  Falls back to the bare name if the front-end
        // didn't capture the raw form.
        d.semantic  = p.rawSemanticName.empty() ? p.semanticName : p.rawSemanticName;
        d.type      = cgTypeForIRType(p.type);
        d.paramno   = static_cast<uint32_t>(i);

        const bool isSampler = (p.type.baseType == IRType::Sampler2D ||
                                p.type.baseType == IRType::SamplerCube);

        if (p.storage == StorageQualifier::Uniform && isSampler)
        {
            d.res       = kCgTexUnit0 + nextSamplerUnit++;
            d.var       = kCgUniform;
            d.direction = kCgIn;
        }
        else if (p.storage == StorageQualifier::Uniform)
        {
            d.res       = 0;  // const-bank resource code TBD
            d.var       = kCgUniform;
            d.direction = kCgIn;
        }
        else
        {
            d.var = kCgVarying;
            const bool isOut = (p.storage == StorageQualifier::Out);
            d.direction = isOut ? kCgOut : kCgIn;
            d.res       = fpResourceFor(toUpper(p.semanticName), p.semanticIndex);
        }
        params.push_back(d);
    }

    // Append one synthetic $kill_NNNN parameter per #pragma alphakill
    // sampler (in source-order, which sce-cgc preserves and indexes
    // sequentially regardless of the original sampler's position).  These
    // params have no name/semantic in the source — sce-cgc generates the
    // name and sets paramno = 0xFFFFFFFF (no user param number).  The
    // unique resource code 0x0cb8 is what the runtime keys off of.
    for (size_t k = 0; k < opts.alphakillSamplers.size(); ++k)
    {
        ParamDesc d;
        // sce-cgc names these `$kill_<4-digit zero-padded index>`.
        // 32-byte buffer covers any practical sampler count without
        // -Wformat-truncation noise.
        char buf[32];
        std::snprintf(buf, sizeof(buf), "$kill_%04zu", k);
        d.name         = buf;
        d.semantic     = "";  // sce-cgc emits no semantic string for these
        d.type         = kCgFloat4;
        d.res          = kCgKillMarker;
        d.var          = kCgVarying;
        d.direction    = kCgOut;
        d.paramno      = kInvalidIndex;
        d.isReferenced = 0;
        params.push_back(d);
    }

    // ----- Build the strings region in sce-cgc's order: per param,
    // emit semantic (if non-empty) then name.  Track each string's
    // offset relative to the start of the container. -----
    const uint32_t headerSize     = 0x20u;
    const uint32_t paramTableSize = static_cast<uint32_t>(params.size() * 48u);
    const uint32_t stringsStart   = headerSize + paramTableSize;

    std::vector<uint8_t> stringsBlob;
    struct StringSlots { uint32_t semanticOffset = 0; uint32_t nameOffset = 0; };
    std::vector<StringSlots> slots(params.size());

    for (size_t i = 0; i < params.size(); ++i)
    {
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

    // CgBinaryFragmentProgram subtype = 22 bytes; pad to 16-byte
    // boundary so ucode lands aligned.
    constexpr uint32_t programSubtypeBytes = 22u;
    const uint32_t ucodeOffset =
        align16(programOffset + programSubtypeBytes);

    const uint32_t ucodeSize  = static_cast<uint32_t>(ucode.size() * 4u);
    const uint32_t totalSize  = ucodeOffset + ucodeSize;

    // ----- Emit the container. -----
    auto& out = result.bytes;
    out.reserve(totalSize);

    // Header.
    put32(out, kProfileFpRsx);
    put32(out, kBinaryFormatRevision);
    put32(out, totalSize);
    put32(out, static_cast<uint32_t>(params.size()));
    put32(out, headerSize);              // parameterArray
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
        put32(out, kInvalidIndex);              // resIndex (-1: not allocated by compiler)
        put32(out, slots[i].nameOffset);        // 0 if no name
        put32(out, 0);                          // defaultValue
        put32(out, 0);                          // embeddedConst
        put32(out, slots[i].semanticOffset);    // 0 if no semantic
        put32(out, d.direction);
        put32(out, d.paramno);
        put32(out, d.isReferenced);
        put32(out, 0);                          // isShared
    }

    // Strings (already absolute offsets baked into the param table).
    out.insert(out.end(), stringsBlob.begin(), stringsBlob.end());
    padTo(out, 16);

    // CgBinaryFragmentProgram subtype.
    put32(out, static_cast<uint32_t>(ucode.size() / 4));     // instructionCount
    put32(out, attrs.attributeInputMask);
    put32(out, attrs.partialTexType);
    put16(out, attrs.texCoordsInputMask);
    put16(out, attrs.texCoords2D);
    put16(out, attrs.texCoordsCentroid);
    out.push_back(attrs.registerCount);
    out.push_back(attrs.outputFromH0);
    out.push_back(attrs.depthReplace);
    out.push_back(attrs.pixelKill);
    padTo(out, 16);

    // ucode: words come from FpAssembler in on-disk halfword-swapped
    // form (matches sce-cgc).  Container is BE, ucode is treated as
    // raw u32 BE.
    for (uint32_t w : ucode) put32(out, w);

    if (out.size() != totalSize)
    {
        result.diagnostics.push_back(
            "sony-fp: emitted " + std::to_string(out.size()) +
            " bytes, expected " + std::to_string(totalSize));
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace sony
