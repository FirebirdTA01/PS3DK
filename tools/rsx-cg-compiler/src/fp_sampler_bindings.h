#ifndef RSX_CG_FP_SAMPLER_BINDINGS_H
#define RSX_CG_FP_SAMPLER_BINDINGS_H

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir.h"
#include "array_uniforms.h"

namespace rsx_cg {

// Both lowering paths and reflection consume this layout. Reserving explicit
// units in only one of them produces valid containers that sample the wrong
// texture. Classify the surviving IR, after dead-fetch/alpha-kill decisions:
// an unused declaration neither reserves a unit nor makes an invalid unit fatal.
struct FpSamplerLayout {
    std::unordered_map<IRValueID, int> units;
    std::unordered_set<IRValueID> used;
    std::vector<std::string> diagnostics;
    std::unordered_map<IRValueID, std::vector<int>> arrayUnits;
    std::unordered_map<IRValueID, std::vector<bool>> arrayUsed;
    std::unordered_map<IRValueID, int> arrayLoads;

    int unit(IRValueID id) const {
        const auto it = units.find(id);
        return it == units.end() ? -1 : it->second;
    }
    int arrayUnit(IRValueID id, unsigned element) const {
        const auto it = arrayUnits.find(id);
        return it == arrayUnits.end() || element >= it->second.size() ? -1 : it->second[element];
    }
    bool arrayElementUsed(IRValueID id, unsigned element) const {
        const auto it = arrayUsed.find(id);
        return it != arrayUsed.end() && element < it->second.size() && it->second[element];
    }
};

template<class Declaration>
inline int explicitFpSamplerUnit(const Declaration& declaration) {
    if (declaration.explicitRegisterBank == 'S')
        return declaration.explicitRegisterIndex;
    std::string semantic = declaration.semanticName;
    for (char& c : semantic)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return semantic == "TEXUNIT" ? declaration.semanticIndex : -1;
}

// Array loads identify an element by declaration ID and constant index. Keep
// that identity through unit allocation, lowering and container reflection.
inline FpSamplerLayout buildFpArraySamplerLayout(const IRModule& module,
                                                const IRFunction& entry) {
    struct Binding {
        IRValueID root;
        int element;
        int explicitUnit;
        std::string name;
    };
    FpSamplerLayout layout;
    std::vector<Binding> bindings;
    std::unordered_map<IRValueID, size_t> identity;
    std::unordered_map<IRValueID, size_t> arrayBase;
    std::unordered_map<std::string, IRValueID> globals;
    const auto add = [&](const auto& declaration) {
        if (declaration.storage != StorageQualifier::Uniform ||
            !isSamplerIRType(declaration.type.baseType)) return;
        const int explicitUnit = explicitFpSamplerUnit(declaration);
        if (!declaration.type.isArray()) {
            identity[declaration.valueId] = bindings.size();
            bindings.push_back({declaration.valueId, -1, explicitUnit, declaration.name});
            return;
        }
        if (declaration.type.baseType != IRType::Sampler2D) {
            layout.diagnostics.push_back("fragment sampler arrays require sampler2D elements; refusing");
            return;
        }
        arrayBase[declaration.valueId] = bindings.size();
        layout.arrayUnits[declaration.valueId].assign(declaration.type.arraySize, -1);
        layout.arrayUsed[declaration.valueId].assign(declaration.type.arraySize, false);
        for (int k = 0; k < declaration.type.arraySize; ++k)
            bindings.push_back({declaration.valueId, k, explicitUnit,
                                declaration.name + "[" + std::to_string(k) + "]"});
    };
    for (const auto& p : entry.parameters) add(p);
    for (const auto& g : module.globals) {
        add(g);
        if (isSamplerIRType(g.type.baseType)) globals[g.name] = g.valueId;
    }
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& ip : block->instructions) {
            if (!ip || ip->op != IROp::LoadUniform) continue;
            const auto array = arrayBase.find(ip->uniformSource);
            if (array != arrayBase.end()) {
                int element = ip->componentIndex;
                const bool constant = ip->arrayIndexKind == IRInstruction::ArrayIndexKind::Constant ||
                    (ip->arrayIndexKind == IRInstruction::ArrayIndexKind::Dynamic &&
                     !ip->operands.empty() && foldConstantIndex(entry, ip->operands[0], element));
                if (!constant || element < 0 ||
                    static_cast<size_t>(element) >= layout.arrayUnits[ip->uniformSource].size()) {
                    layout.diagnostics.push_back("fragment sampler array requires an in-range constant index; refusing");
                    continue;
                }
                identity[ip->result] = array->second + static_cast<size_t>(element);
            } else {
                const auto g = globals.find(ip->targetName);
                if (g != globals.end() && identity.count(g->second))
                    identity[ip->result] = identity.at(g->second);
            }
        }
    }
    std::vector<size_t> firstUse;
    std::unordered_set<size_t> used;
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& ip : block->instructions) {
            if (!ip) continue;
            switch (ip->op) {
            case IROp::TexSample: case IROp::TexSampleProj:
            case IROp::TexSampleLod: case IROp::TexSampleGrad:
            case IROp::TexSampleBias: case IROp::TexFetch: break;
            default: continue;
            }
            if (ip->operands.empty()) continue;
            const auto binding = identity.find(ip->operands[0]);
            if (binding == identity.end()) {
                layout.diagnostics.push_back("fragment texture unit: sampler operand has no known element binding; refusing");
            } else if (used.insert(binding->second).second) {
                firstUse.push_back(binding->second);
            }
        }
    }
    std::array<bool, 16> reserved{};
    std::vector<int> assigned(bindings.size(), -1);
    for (size_t i = 0; i < bindings.size(); ++i) {
        const int n = bindings[i].explicitUnit;
        if (n >= 0 && n < 16) assigned[i] = n;
        if (!used.count(i) || n < 0) continue;
        if (n >= 16)
            layout.diagnostics.push_back("fragment texture unit outside 0..15 for sampler '" + bindings[i].name + "'; refusing");
        else reserved[static_cast<size_t>(n)] = true;
    }
    for (size_t i : firstUse) {
        if (bindings[i].explicitUnit >= 0) continue;
        int n = 0;
        while (n < 16 && reserved[static_cast<size_t>(n)]) ++n;
        if (n == 16)
            layout.diagnostics.push_back("fragment texture unit allocation exhausted for sampler '" + bindings[i].name + "'; refusing");
        else {
            reserved[static_cast<size_t>(n)] = true;
            assigned[i] = n;
        }
    }
    for (size_t i = 0; i < bindings.size(); ++i) {
        const auto& b = bindings[i];
        if (used.count(i)) layout.used.insert(b.root);
        if (b.element < 0) layout.units[b.root] = assigned[i];
        else {
            layout.arrayUnits[b.root][b.element] = assigned[i];
            layout.arrayUsed[b.root][b.element] = used.count(i) != 0;
        }
    }
    for (const auto& id : identity)
        if (bindings[id.second].element >= 0)
            layout.arrayLoads[id.first] = assigned[id.second];
    return layout;
}

inline FpSamplerLayout buildFpSamplerLayout(const IRModule& module,
                                           const IRFunction& entry) {
    const auto arraySampler = [](const auto& d) {
        return d.storage == StorageQualifier::Uniform && d.type.isArray() &&
               isSamplerIRType(d.type.baseType);
    };
    if (std::any_of(entry.parameters.begin(), entry.parameters.end(), arraySampler) ||
        std::any_of(module.globals.begin(), module.globals.end(), arraySampler))
        return buildFpArraySamplerLayout(module, entry);
    struct Sampler { IRValueID id; std::string name; int explicitUnit; };
    FpSamplerLayout layout;
    std::vector<Sampler> samplers;
    std::unordered_map<std::string, IRValueID> globals;
    std::unordered_map<IRValueID, IRValueID> identity;
    const auto add = [&](const auto& d) {
        if (d.storage != StorageQualifier::Uniform ||
            !isSamplerIRType(d.type.baseType)) return;
        samplers.push_back({d.valueId, d.name, explicitFpSamplerUnit(d)});
        identity[d.valueId] = d.valueId;
    };
    for (const auto& p : entry.parameters) add(p);
    for (const auto& g : module.globals) {
        add(g);
        if (isSamplerIRType(g.type.baseType)) globals[g.name] = g.valueId;
    }
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& inst : block->instructions) {
            if (!inst || inst->op != IROp::LoadUniform) continue;
            const auto g = globals.find(inst->targetName);
            if (g != globals.end()) identity[inst->result] = g->second;
        }
    }
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& inst : block->instructions) {
            if (!inst) continue;
            // Keep this texture-op list in step with DeadCodeElimination's
            // m_preserveTextureReads cases in ir_passes.cpp. A new fetch
            // (e.g. TexSampleBias) must count here when alpha-kill preserves it.
            switch (inst->op) {
            case IROp::TexSample: case IROp::TexSampleProj:
            case IROp::TexSampleLod: case IROp::TexSampleGrad:
            case IROp::TexSampleBias: case IROp::TexFetch: break;
            default: continue;
            }
            if (inst->operands.empty()) continue; // lowering diagnoses arity
            const auto sampler = identity.find(inst->operands[0]);
            if (sampler == identity.end()) {
                layout.diagnostics.push_back("fragment texture unit: sampler operand %" +
                    std::to_string(inst->operands[0]) + " has no known binding; refusing");
            } else {
                layout.used.insert(sampler->second);
            }
        }
    }
    std::array<bool, 16> reserved{};
    for (const auto& s : samplers) {
        const bool used = layout.used.count(s.id) != 0;
        const int n = s.explicitUnit;
        layout.units[s.id] = n >= 0 && n < 16 ? n : -1;
        if (!used || n < 0) continue;
        if (n >= 16) {
            layout.diagnostics.push_back("fragment texture unit " + std::to_string(n) +
                " for sampler '" + s.name + "' is outside 0..15; refusing");
        } else {
            reserved[static_cast<size_t>(n)] = true;
        }
    }
    // Preserve relative declaration order among used implicit samplers. The
    // established compiler can use first-use order; that existing byte-layout gap is
    // independent of honouring explicit bindings and keeping metadata coherent.
    for (const auto& s : samplers) {
        if (!layout.used.count(s.id) || s.explicitUnit >= 0) continue;
        int n = 0;
        while (n < 16 && reserved[static_cast<size_t>(n)]) ++n;
        if (n == 16) {
            layout.diagnostics.push_back("fragment texture unit allocation exhausted for sampler '" +
                s.name + "'; refusing");
        } else {
            reserved[static_cast<size_t>(n)] = true;
            layout.units[s.id] = n;
        }
    }
    return layout;
}

// The vertex side of the same contract, with the vertex unit's own rules
// (measured on the established compiler, sce_vp_rsx):
//   - four texture units; TXL carries the unit in two bits;
//   - implicit units go to used samplers in FIRST-USE order, lowest free
//     unit first, around the explicit ones (declared a-then-b, b fetched
//     first: b gets unit 0);
//   - a USED sampler bound to TEXUNIT4 or above is refused (C5102); an
//     unused one is accepted and simply has no unit;
//   - an unused sampler bound in range keeps its unit (isReferenced 0);
//   - a fifth used sampler is refused (C6012).
// Kept separate from the fragment layout: that one allocates in declaration
// order, and the fragment programs that depend on it must not move.
inline FpSamplerLayout buildVpSamplerLayout(const IRModule& module,
                                           const IRFunction& entry) {
    constexpr int kVpTextureUnits = 4;
    struct Sampler { IRValueID id; std::string name; int explicitUnit; };
    FpSamplerLayout layout;
    std::vector<Sampler> samplers;
    std::unordered_map<std::string, IRValueID> globals;
    std::unordered_map<IRValueID, IRValueID> identity;
    const auto add = [&](const auto& d) {
        if (d.storage != StorageQualifier::Uniform ||
            !isSamplerIRType(d.type.baseType)) return;
        samplers.push_back({d.valueId, d.name, explicitFpSamplerUnit(d)});
        identity[d.valueId] = d.valueId;
    };
    for (const auto& p : entry.parameters) add(p);
    for (const auto& g : module.globals) {
        add(g);
        if (isSamplerIRType(g.type.baseType)) globals[g.name] = g.valueId;
    }
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& inst : block->instructions) {
            if (!inst || inst->op != IROp::LoadUniform) continue;
            const auto g = globals.find(inst->targetName);
            if (g != globals.end()) identity[inst->result] = g->second;
        }
    }
    std::vector<IRValueID> firstUse;
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& inst : block->instructions) {
            if (!inst) continue;
            switch (inst->op) {
            case IROp::TexSample: case IROp::TexSampleProj:
            case IROp::TexSampleLod: case IROp::TexSampleGrad:
            case IROp::TexSampleBias: case IROp::TexFetch: break;
            default: continue;
            }
            if (inst->operands.empty()) continue; // lowering diagnoses arity
            const auto sampler = identity.find(inst->operands[0]);
            if (sampler == identity.end()) {
                layout.diagnostics.push_back("vertex texture unit: sampler operand %" +
                    std::to_string(inst->operands[0]) + " has no known binding; refusing");
            } else if (layout.used.insert(sampler->second).second) {
                firstUse.push_back(sampler->second);
            }
        }
    }
    std::array<bool, kVpTextureUnits> reserved{};
    for (const auto& s : samplers) {
        const bool used = layout.used.count(s.id) != 0;
        const int n = s.explicitUnit;
        layout.units[s.id] = n >= 0 && n < kVpTextureUnits ? n : -1;
        if (!used || n < 0) continue;
        if (n >= kVpTextureUnits) {
            layout.diagnostics.push_back(
                "vertex texture unit: error C5102: input semantic attribute "
                "\"TEXUNIT\" has too big of a numeric index (" + std::to_string(n) +
                ") on sampler '" + s.name + "'; the vertex unit has TEXUNIT0..3; refusing");
        } else {
            reserved[static_cast<size_t>(n)] = true;
        }
    }
    for (IRValueID id : firstUse) {
        const auto s = std::find_if(samplers.begin(), samplers.end(),
            [&](const Sampler& candidate) { return candidate.id == id; });
        if (s == samplers.end() || s->explicitUnit >= 0) continue;
        int n = 0;
        while (n < kVpTextureUnits && reserved[static_cast<size_t>(n)]) ++n;
        if (n == kVpTextureUnits) {
            layout.diagnostics.push_back(
                "vertex texture unit: error C6012: Sampler limit exceeded; more than 4 "
                "samplers needed to compile program (no unit left for sampler '" +
                s->name + "'); refusing");
            break;
        }
        reserved[static_cast<size_t>(n)] = true;
        layout.units[id] = n;
    }
    return layout;
}

} // namespace rsx_cg
#endif
