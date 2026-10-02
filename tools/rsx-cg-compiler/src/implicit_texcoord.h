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
#include <variant>
#include <set>
#include <string>
#include <vector>

namespace rsx_cg {

// A select on a constant condition is its chosen operand.  Measured: an
// input read only under `if (false)`, `false ? u : k` or a `static const
// bool` that is false is UNREAD on the reference (UNDEFINED, no index), but
// such a select survives into the backend here (review: codex), keeping
// the dead load alive.  Fold it before deciding what is read; returns
// whether anything changed (the caller re-runs dead-code elimination).
enum class SelectFold { Unchanged, Changed, Failed };

inline SelectFold foldConstantSelects(IRFunction& entry)
{
    // SSA is acyclic, so the number of selects bounds both a replacement
    // chain and the rounds to a fixed point.  Reaching that bound means a
    // cycle or a bug: report it (the caller refuses by name) rather than
    // return an intermediate value whose dead arm stays live and shifts an
    // implicit TEXCOORD (review: codex - fixed caps of 64/16 did exactly
    // that on a 65-deep chain).
    size_t selects = 0;
    for (const auto& block : entry.blocks)
        if (block)
            for (const auto& inst : block->instructions)
                if (inst && inst->op == IROp::Select) ++selects;
    if (selects == 0) return SelectFold::Unchanged;
    const size_t bound = selects + 1;

    std::map<IRValueID, IRValueID> chosen;
    bool failed = false;
    const auto resolve = [&](IRValueID v) {
        for (size_t step = 0; step <= bound; ++step) {
            const auto it = chosen.find(v);
            if (it == chosen.end()) return v;
            v = it->second;
        }
        failed = true;   // longer than the number of selects: a cycle
        return v;
    };
    // To a fixed point: a condition may itself be a select that an earlier
    // round folded to a constant - `bool b = true ? false : (u.x > 0);
    // b ? u : k` (review: codex; measured u UNREAD on the reference).
    bool converged = false;
    for (size_t round = 0; round <= bound && !failed; ++round) {
        bool grew = false;
        for (const auto& block : entry.blocks) {
            if (!block) continue;
            for (const auto& inst : block->instructions) {
                if (!inst || inst->op != IROp::Select || inst->operands.size() != 3) continue;
                if (chosen.count(inst->result)) continue;
                const auto* c = dynamic_cast<const IRConstant*>(entry.getValue(resolve(inst->operands[0])));
                if (!c || !c->type.isScalar()) continue;   // a vector condition selects per lane
                bool truth;
                if (std::holds_alternative<bool>(c->value)) truth = std::get<bool>(c->value);
                else if (std::holds_alternative<int32_t>(c->value)) truth = std::get<int32_t>(c->value) != 0;
                else if (std::holds_alternative<uint32_t>(c->value)) truth = std::get<uint32_t>(c->value) != 0;
                else if (std::holds_alternative<float>(c->value)) truth = std::get<float>(c->value) != 0.0f;
                else continue;
                chosen[inst->result] = truth ? inst->operands[1] : inst->operands[2];
                grew = true;
            }
        }
        if (!grew) { converged = true; break; }
    }
    if (failed || !converged) return SelectFold::Failed;
    if (chosen.empty()) return SelectFold::Unchanged;
    // Path-compress every replacement to its final value, then rewrite.
    std::map<IRValueID, IRValueID> finalOf;
    for (const auto& kv : chosen) finalOf[kv.first] = resolve(kv.first);
    if (failed) return SelectFold::Failed;
    for (auto& block : entry.blocks) {
        if (!block) continue;
        for (auto& inst : block->instructions) {
            if (!inst) continue;
            for (IRValueID& op : inst->operands) {
                const auto it = finalOf.find(op);
                if (it != finalOf.end()) op = it->second;
            }
        }
    }
    return SelectFold::Changed;
}

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
