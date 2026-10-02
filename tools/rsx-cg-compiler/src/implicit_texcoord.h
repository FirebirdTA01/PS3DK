#pragma once
// Implicit TEXCOORD binding from the inputs a fragment program READS
// (t_3289f98f).
//
// Measured on sce-cgc 475: a semantic-less fragment input - a bare
// parameter or a struct member, each instance on its own - takes the lowest
// TEXCOORD<N> no explicit input claims, in declaration order, but ONLY when
// the program reads it.  An unread one is a varying record with an
// UNDEFINED resource and consumes no index:
//
//   float4 main(float2 u, float2 k) : COLOR reading k only   u UNDEF, k TEX0
//   struct d { float2 a; float2 b; }; main(d v) reading v.b   v.a UNDEF, v.b TEX0
//   struct S { float2 uv; }; main(S a, S b, S c), b.uv c.uv   a.uv UNDEF, b TEX0, c TEX1
//
// The semantic pass assigned provisional indices in declaration order
// regardless of use (and kept them on the shared struct field), so the
// program read TEXCOORD1 where the application feeds TEXCOORD0 - a wrong
// varying with exit 0.  This runs after dead-code elimination, when "read"
// means "still loaded", and rewrites every implicit binding from the
// recorded declaration order.

#include "ir.h"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace rsx_cg {

inline void bindImplicitTexCoords(IRFunction& entry,
                                  const std::vector<std::string>& order,
                                  const std::set<int>& explicitTexCoords)
{
    if (order.empty()) return;

    // Which implicit inputs survive: a member by its load's qualified name,
    // a bare parameter by any remaining use of its value.
    std::set<std::string> read;
    std::set<IRValueID> used;
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& inst : block->instructions) {
            if (!inst) continue;
            for (IRValueID op : inst->operands) used.insert(op);
            if ((inst->op == IROp::LoadAttribute || inst->op == IROp::LoadVarying) &&
                inst->inferredSemantic && !inst->structParamName.empty())
                read.insert(inst->structParamName + "." + inst->fieldName);
        }
    }
    for (const auto& p : entry.parameters)
        if (p.inferredSemantic && used.count(p.valueId))
            read.insert(p.name);

    // Lowest free index, declaration order, explicit TEXCOORDs reserved.
    std::map<std::string, int> index;
    int next = 0;
    for (const std::string& name : order) {
        if (!read.count(name) || index.count(name)) continue;
        while (explicitTexCoords.count(next)) ++next;
        index[name] = next++;
    }

    for (auto& block : entry.blocks) {
        if (!block) continue;
        for (auto& inst : block->instructions) {
            if (!inst || !inst->inferredSemantic || inst->structParamName.empty()) continue;
            if (inst->op != IROp::LoadAttribute && inst->op != IROp::LoadVarying) continue;
            const auto it = index.find(inst->structParamName + "." + inst->fieldName);
            if (it == index.end()) continue;   // not an implicit TEXCOORD (e.g. an array element)
            inst->semanticIndex = it->second;
            inst->rawSemanticName = "TEXCOORD" + std::to_string(it->second);
        }
    }
    for (auto& p : entry.parameters) {
        if (!p.inferredSemantic || p.semanticName != "TEXCOORD") continue;
        const auto it = index.find(p.name);
        if (it != index.end()) {
            p.semanticIndex = it->second;
            p.rawSemanticName = "TEXCOORD" + std::to_string(it->second);
        } else {
            // Unread: no TEXCOORD; the container records it UNDEFINED.
            p.semanticName.clear();
            p.rawSemanticName.clear();
        }
    }
}

} // namespace rsx_cg
