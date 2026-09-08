#ifndef RSX_CG_FP_SAMPLER_BINDINGS_H
#define RSX_CG_FP_SAMPLER_BINDINGS_H

#include <array>
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ir.h"

namespace rsx_cg {

// Both lowering paths and reflection consume this layout. Reserving explicit
// units in only one of them produces valid containers that sample the wrong
// texture. Classify the surviving IR, after dead-fetch/alpha-kill decisions:
// an unused declaration neither reserves a unit nor makes an invalid unit fatal.
struct FpSamplerLayout {
    std::unordered_map<IRValueID, int> units;
    std::unordered_set<IRValueID> used;
    std::vector<std::string> diagnostics;

    int unit(IRValueID id) const {
        const auto it = units.find(id);
        return it == units.end() ? -1 : it->second;
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

inline FpSamplerLayout buildFpSamplerLayout(const IRModule& module,
                                           const IRFunction& entry) {
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
            case IROp::TexFetch: break;
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
    // reference can use first-use order; that existing byte-layout gap is
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

} // namespace rsx_cg
#endif
