#ifndef RSX_CG_COMPILER_ARRAY_UNIFORMS_H
#define RSX_CG_COMPILER_ARRAY_UNIFORMS_H

/*
 * rsx-cg-compiler — array uniforms: the facts three sites must agree on.
 *
 * An array uniform (`uniform float4 u_colors[4]`, as an entry parameter or
 * at file scope) is N ELEMENTS to the container and to the hardware: the
 * reference declares one parameter record per element, named `name[i]`,
 * typed as the element, in declaration order, whether the element is used
 * or not; a used element carries its own resources (FP: its inline-const
 * relocation offsets; VP: its constant register).  The general lowering
 * assigns those resources, the FP emitter numbers the inline-const slots,
 * and the two container writers declare the records - and every one of
 * them has to count and name the elements the same way, or a runtime patch
 * of one element lands in another's constant.  This header is that one
 * way (t_f9ecd3ac).
 */

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ir.h"

namespace rsx_cg
{

// How the entry function reaches each array uniform, by the array's name:
// which elements it loads with a constant index, and whether any load
// chooses the element at run time.  Classified BEFORE any resource is
// assigned, so a run-time load can never meet an array that was already
// laid out per element.
struct ArrayUniformUse
{
    std::set<int> constantElements;
    bool          dynamic = false;
};
using ArrayUniformUses = std::map<std::string, ArrayUniformUse>;

inline ArrayUniformUses classifyArrayUniformUses(const IRFunction& entry)
{
    ArrayUniformUses uses;
    for (const auto& blockPtr : entry.blocks)
    {
        if (!blockPtr) continue;
        for (const auto& instPtr : blockPtr->instructions)
        {
            if (!instPtr || instPtr->op != IROp::LoadUniform) continue;
            const IRInstruction& in = *instPtr;
            using Kind = IRInstruction::ArrayIndexKind;
            if (in.arrayIndexKind == Kind::Constant)
                uses[in.targetName].constantElements.insert(in.componentIndex);
            else if (in.arrayIndexKind == Kind::Dynamic)
                uses[in.targetName].dynamic = true;
        }
    }
    return uses;
}

// The element record's name.  One spelling: the container's strings blob
// is laid out in parameter order with 16-byte-aligned inline-constant
// records after each name, so a different rendering of one element name
// would shift every later record's bytes, not just its own string.
inline std::string arrayElementName(const std::string& name, int index)
{
    return name + "[" + std::to_string(index) + "]";
}

// An FP uniform takes one inline-const slot per element; a VP uniform one
// constant register per REFERENCED element (or a contiguous block once
// run-time indexing lands).  Samplers take neither and are excluded by the
// callers, as they are today.
inline unsigned fpUniformSlotCount(const IRTypeInfo& type)
{
    return type.isArray() && type.arraySize > 0
               ? static_cast<unsigned>(type.arraySize) : 1u;
}

// FP inline-const slot numbering.  Every entry parameter takes one slot
// (its own index, so a program with no arrays keeps the numbering it has
// always had), except an array uniform parameter, which takes one per
// element; file-scope uniforms continue after the last parameter's slots.
// The lowering, the FP emitter and the FP container all number from here.
inline std::vector<unsigned> fpParameterSlotBases(const IRFunction& entry)
{
    std::vector<unsigned> bases;
    bases.reserve(entry.parameters.size());
    unsigned cursor = 0;
    for (const auto& p : entry.parameters)
    {
        bases.push_back(cursor);
        cursor += (p.storage == StorageQualifier::Uniform && p.type.isArray())
                      ? fpUniformSlotCount(p.type) : 1u;
    }
    return bases;
}

inline unsigned fpFirstGlobalSlot(const IRFunction& entry)
{
    const auto bases = fpParameterSlotBases(entry);
    if (bases.empty()) return 0;
    const auto& last = entry.parameters.back();
    return bases.back() +
           ((last.storage == StorageQualifier::Uniform && last.type.isArray())
                ? fpUniformSlotCount(last.type) : 1u);
}

// The element types laid out per element: float and half scalars and
// vectors (a half element is an inline-const block like a float one, and
// the reference records it as float / float4).  A matrix element (the
// reference accepts float4x4 m[2]) and anything nested keep a named
// refusal until they are measured and pinned.
inline bool arrayElementLowered(const IRTypeInfo& type)
{
    return type.isArray() && !type.isMatrix() &&
           (type.elementType == IRType::Float32 ||
            type.elementType == IRType::Float16) &&
           type.vectorSize >= 1 && type.vectorSize <= 4;
}

}  // namespace rsx_cg

#endif  /* RSX_CG_COMPILER_ARRAY_UNIFORMS_H */
