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

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <variant>

#include "ir.h"

namespace rsx_cg
{

// The integer an index VALUE folds to, when it is a constant the builder
// did not see as one: `int i = 1; u[i]` reaches the load as a run-time
// index whose operand is the constant 1, and `float i = 1; u[int(i)]` as
// a float-to-int of the constant 1.0.  The reference treats both as the
// CONSTANT index 1 - per-element layout, no address register, on both
// profiles - and refuses an out-of-range one (C1068), so the fold has to
// happen where the layout is classified, before any register is handed
// out (review, t_99b29225: two literal indices had shared one lane).
inline bool foldConstantIndex(const IRFunction& entry, IRValueID id,
                              int& out)
{
    const auto fromConstant = [&](IRValueID cid) {
        const auto* constant =
            dynamic_cast<const IRConstant*>(entry.getValue(cid));
        if (!constant) return false;
        if (std::holds_alternative<int32_t>(constant->value))
            out = std::get<int32_t>(constant->value);
        else if (std::holds_alternative<uint32_t>(constant->value))
            out = static_cast<int>(std::get<uint32_t>(constant->value));
        else if (std::holds_alternative<bool>(constant->value))
            out = std::get<bool>(constant->value) ? 1 : 0;
        else if (std::holds_alternative<float>(constant->value))
            out = static_cast<int>(std::get<float>(constant->value));  // truncates, as int() does
        else
            return false;
        return true;
    };
    if (fromConstant(id)) return true;
    for (const auto& blockPtr : entry.blocks)
    {
        if (!blockPtr) continue;
        for (const auto& instPtr : blockPtr->instructions)
        {
            if (!instPtr || instPtr->result != id) continue;
            if (instPtr->op == IROp::FloatToInt && !instPtr->operands.empty())
                return fromConstant(instPtr->operands[0]);
            return false;
        }
    }
    return false;
}

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
            int folded = 0;
            if (in.arrayIndexKind == Kind::Constant)
                uses[in.targetName].constantElements.insert(in.componentIndex);
            else if (in.arrayIndexKind == Kind::Dynamic &&
                     !in.operands.empty() &&
                     foldConstantIndex(entry, in.operands[0], folded))
                uses[in.targetName].constantElements.insert(folded);
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

// The VP constant registers of an array's elements, in element order,
// -1 for an element that holds none.  `cursor` is the descending uniform
// walk (c467 first) and is advanced past what the array took.  Two
// shapes, both measured on the reference (t_f9ecd3ac, t_99b29225):
//   - constant indices only: a register per REFERENCED element, taken
//     from the cursor in ASCENDING element order (u_bones[1] -> c467,
//     u_bones[3] -> c466); unreferenced elements hold none;
//   - any run-time index: a CONTIGUOUS block of all N, consecutive and
//     ascending, taken at the array's place in the walk (u_bones[4] alone
//     -> c464..c467; after u_scale = c467 and u_a[1] = c466, u_b[3] ->
//     c463..c465), so the address register can offset into it.  Every
//     element is referenced, the constant-index reads included.
// The lowering assigns from here and the container declares from here;
// they used to each walk the cursor on their own.
inline std::vector<int> vpArrayElementRegisters(const ArrayUniformUse& use,
                                                int count, int& cursor)
{
    std::vector<int> regs(static_cast<size_t>(std::max(0, count)), -1);
    if (use.dynamic)
    {
        cursor -= count;
        for (int k = 0; k < count; ++k)
            regs[static_cast<size_t>(k)] = cursor + 1 + k;
        return regs;
    }
    for (int k : use.constantElements)
    {
        if (k < 0 || k >= count) continue;
        regs[static_cast<size_t>(k)] = cursor--;
    }
    return regs;
}

// An FP uniform takes one inline-const slot per element; a VP uniform one
// constant register per REFERENCED element, or a contiguous block of all
// of them under a run-time index (vpArrayElementRegisters).  Samplers take
// neither and are excluded by the callers, as they are today.
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
