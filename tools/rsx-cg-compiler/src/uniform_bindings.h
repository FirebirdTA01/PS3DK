#ifndef RSX_CG_COMPILER_UNIFORM_BINDINGS_H
#define RSX_CG_COMPILER_UNIFORM_BINDINGS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "array_uniforms.h"

namespace rsx_cg {

struct ExplicitUniformBinding {
    std::string semantic;
    // One base per array element, or one for an ordinary scalar/vector/matrix.
    // Unreferenced elements have no register. Matrix rows follow their base.
    std::vector<int> registers;
};

struct VpExplicitUniformBindings {
    std::map<IRValueID, ExplicitUniformBinding> uniforms;
    std::vector<std::string> diagnostics;

    const ExplicitUniformBinding* find(IRValueID owner) const {
        const auto it = uniforms.find(owner);
        return it == uniforms.end() ? nullptr : &it->second;
    }
};

// PS3_475, t_49f3cc72: register(CN) and :CN are the same binding, both
// already carried by IR. Explicit pins do not consume either auto cursor.
// A referenced ordinary matrix occupies all rows; an array allocates only
// referenced elements, or all elements for dynamic indexing. In particular,
// M[2]:C252 using only M[0] fits four rows; unused M[1] stays UNDEFINED.
// Used elements must fit c0..255. Unused C256 is legal and stays UNDEFINED.
// Explicit aliases are legal: this is not a disjoint-range allocator.
inline VpExplicitUniformBindings resolveVpExplicitUniformBindings(
    const IRFunction& entry, const IRModule& module)
{
    VpExplicitUniformBindings result;
    const auto add = [&](const auto& uniform) {
        if (uniform.storage != StorageQualifier::Uniform) return;
        const bool explicitBank = uniform.explicitRegisterBank == 'C';
        if (!explicitBank && uniform.semanticName != "C" &&
            uniform.semanticName != "c") return;
        if (result.uniforms.count(uniform.valueId)) return;
        const int base = explicitBank ? uniform.explicitRegisterIndex
                                      : uniform.semanticIndex;
        const int count = uniform.type.isArray() ? uniform.type.arraySize : 1;
        const int rows = uniform.type.isMatrix() ? uniform.type.matrixRows : 1;
        ExplicitUniformBinding binding;
        binding.semantic = "C" + std::to_string(base);
        binding.registers.assign(static_cast<size_t>(std::max(0, count)), -1);
        bool ordinaryUsed = false;
        ArrayUniformUse arrayUse;
        for (const auto& block : entry.blocks) {
            if (!block) continue;
            for (const auto& instruction : block->instructions) {
                if (!instruction) continue;
                if (std::find(instruction->operands.begin(), instruction->operands.end(),
                              uniform.valueId) != instruction->operands.end())
                    ordinaryUsed = true;
                if (instruction->op != IROp::LoadUniform ||
                    instruction->uniformSource != uniform.valueId) continue;
                ordinaryUsed = true;
                using Kind = IRInstruction::ArrayIndexKind;
                int folded = 0;
                if (instruction->arrayIndexKind == Kind::Constant)
                    arrayUse.constantElements.insert(instruction->componentIndex);
                else if (instruction->arrayIndexKind == Kind::Dynamic &&
                         !instruction->operands.empty() &&
                         foldConstantIndex(entry, instruction->operands[0], folded))
                    arrayUse.constantElements.insert(folded);
                else if (instruction->arrayIndexKind == Kind::Dynamic)
                    arrayUse.dynamic = true;
            }
        }
        for (int element = 0; element < count; ++element) {
            const bool used = uniform.type.isArray()
                ? arrayUse.dynamic || arrayUse.constantElements.count(element)
                : ordinaryUsed;
            if (!used) continue;
            const int64_t first = static_cast<int64_t>(base) +
                                  static_cast<int64_t>(element) * rows;
            if (rows <= 0 || first < 0 || first + rows > 256) {
                result.diagnostics.push_back(
                    "nv40-general-vp: explicit constant binding " + binding.semantic +
                    " for '" + uniform.name + "' exceeds c0..c255 for referenced element " +
                    std::to_string(element) + "; refusing");
                continue;
            }
            binding.registers[static_cast<size_t>(element)] = static_cast<int>(first);
        }
        result.uniforms.emplace(uniform.valueId, std::move(binding));
    };
    for (const auto& parameter : entry.parameters) add(parameter);
    for (const auto& global : module.globals) add(global);
    return result;
}

} // namespace rsx_cg
#endif
