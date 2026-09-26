#ifndef RSX_CG_COMPILER_UNIFORM_BINDINGS_H
#define RSX_CG_COMPILER_UNIFORM_BINDINGS_H

#include <cstdint>
#include <cctype>
#include <map>
#include <limits>
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

struct ExplicitUniformBindings {
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
inline ExplicitUniformBindings resolveExplicitUniformBindings(
    const IRFunction& entry, const IRModule& module, bool vertexRegisters)
{
    ExplicitUniformBindings result;
    const auto add = [&](const auto& uniform) {
        if (uniform.storage != StorageQualifier::Uniform) return;
        // A sampler's C<N> is a texture-unit binding in the reference,
        // not CG_C metadata. Its allocation belongs to the sampler layouts
        // (fp_sampler_bindings.h) in both profiles: a vertex sampler takes
        // a texture unit and never a c[] register either, and rewriting
        // its resource here would also corrupt compact CGB.
        if (isSamplerIRType(uniform.type.baseType)) return;
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
        // A semantic spelling keeps its leading zeros: PS3_475 records
        // :C009 as C009, while :c9 and register(c9) both record C9.
        if (!explicitBank && !uniform.rawSemanticName.empty()) {
            binding.semantic = uniform.rawSemanticName;
            std::transform(binding.semantic.begin(), binding.semantic.end(),
                           binding.semantic.begin(), [](unsigned char c) {
                               return static_cast<char>(std::toupper(c));
                           });
        }
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
            // FP C<N> describes reflection, not a physical VP register.
            // PS3_475 accepts FP C256 and matrix rows above it. Only the
            // representation's integer limit applies on the FP side.
            const int64_t limit = vertexRegisters ? 256 :
                static_cast<int64_t>(std::numeric_limits<int>::max()) + 1;
            if (rows <= 0 || first < 0 || first + rows > limit) {
                result.diagnostics.push_back(
                    std::string(vertexRegisters ? "nv40-general-vp: " : "cg-container-fp: ") +
                    "explicit constant binding " + binding.semantic +
                    " for '" + uniform.name + (vertexRegisters
                        ? "' exceeds c0..c255 for referenced element "
                        : "' exceeds representable binding indices for referenced element ") +
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

using VpExplicitUniformBindings = ExplicitUniformBindings;
inline ExplicitUniformBindings resolveVpExplicitUniformBindings(
    const IRFunction& entry, const IRModule& module)
{
    return resolveExplicitUniformBindings(entry, module, true);
}

inline ExplicitUniformBindings resolveFpExplicitUniformBindings(
    const IRFunction& entry, const IRModule& module)
{
    auto result = resolveExplicitUniformBindings(entry, module, false);
    // FP aliases share ownership of embedded-constant patch sites in the
    // reference. Until t_642131af coalesces those sites, exposing independent
    // blocks as one binding would give a runtime patch the wrong reach.
    // Unused aliases carry no sites and remain legal (oracle u-only/v-only).
    std::map<int, std::string> usedRegisters;
    const auto check = [&](const auto& uniform) {
        const auto* binding = result.find(uniform.valueId);
        if (!binding) return;
        const int rows = uniform.type.isMatrix() ? uniform.type.matrixRows : 1;
        for (int base : binding->registers) {
            if (base < 0) continue;
            for (int row = 0; row < rows; ++row) {
                const auto inserted = usedRegisters.emplace(base + row, uniform.name);
                if (!inserted.second) {
                    result.diagnostics.push_back(
                        "cg-container-fp: overlapping used FP constant bindings for '" +
                        inserted.first->second + "' and '" + uniform.name + "' at C" +
                        std::to_string(base + row) +
                        "; interim refusal until shared embedded patch ownership (t_642131af)");
                    return;
                }
            }
        }
    };
    for (const auto& parameter : entry.parameters) check(parameter);
    for (const auto& global : module.globals) check(global);
    return result;
}

} // namespace rsx_cg
#endif
