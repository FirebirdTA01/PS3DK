#pragma once
// Implicit vertex-attribute binding from the inputs a vertex program READS.
//
// Measured on sce-cgc 475: a semantic-less vertex input - a bare parameter
// or a varying struct member, each instance on its own - takes the lowest
// ATTR<N> no explicit input semantic claims (POSITION = ATTR0, NORMAL =
// ATTR2, COLOR = ATTR3, ...), in declaration order across the parameters
// and their members, but ONLY when the program reads it.  An unread one is
// declared with no resource (0xcb8) and consumes no index:
//
//   struct S { a; b; c; d; }, reading a and c          a ATTR0, c ATTR1
//   struct S { a..f }, col : COLOR, all read            a0 b1 c2 d4 e5 f6
//   main(float4 x, float4 y, float4 z), y unread        x ATTR0, z ATTR1
//   main(S{a,b} s, T{c,d} t), a unread                  s.b 0, t.c 1, t.d 2
//   struct S { a; b : NORMAL; c; }                      a0 b2 c1
//
// Ours bound a member to its position in the struct and a bare parameter
// to its position among the bare parameters, so `d` landed on ATTR3 beside
// a COLOR input and `c` on ATTR2 beside a NORMAL one: two inputs reading
// one attribute, with exit 0.  This runs after dead-code elimination, when
// "read" means "still loaded", and rewrites every implicit binding.

#include "ir.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>

namespace rsx_cg {

// The ATTR index an explicit vertex-input semantic binds, -1 for none.
// The one table: the container's resource codes are ATTR0 + this.
inline int vpInputAttrIndex(const std::string& semUpper, int semIndex)
{
    if (semUpper.empty() && semIndex >= 0 && semIndex < 16) return semIndex;
    if (semUpper == "ATTR" && semIndex >= 0 && semIndex < 16) return semIndex;
    if (semUpper == "POSITION") return 0;
    if (semUpper == "NORMAL")   return 2;
    if (semUpper == "COLOR" || semUpper == "COL") return semIndex == 1 ? 4 : 3;
    // Index-less aliases of COLOR0/COLOR1; DIFFUSE1 stays unmapped.
    if (semUpper == "DIFFUSE"  && semIndex == 0) return 3;
    if (semUpper == "SPECULAR" && semIndex == 0) return 4;
    if (semUpper == "BLENDWEIGHT" || semUpper == "WEIGHT") return 1;
    if (semUpper == "BLENDINDICES" || semUpper == "INDICES") return 7;
    if (semUpper == "TEXCOORD" || semUpper == "TEX") return 8 + semIndex;
    if (semUpper == "TANGENT" && semIndex == 0) return 14;
    if (semUpper == "BINORMAL" && semIndex == 0) return 15;
    return -1;
}

inline std::string upperSemantic(std::string s)
{
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// An implicit bare parameter: the semantic pass gave it a provisional ATTR.
inline bool isImplicitVertexParam(const IRParameter& p)
{
    return p.inferredSemantic && p.semanticName == "ATTR" &&
           (p.storage == StorageQualifier::In || p.storage == StorageQualifier::None) &&
           !p.type.isArray() && !p.type.isMatrix();
}

// A varying matrix needs row loads and row records, neither modelled yet.
// Run after DCE: an unread explicit matrix among explicit neighbours works
// today and must remain accepted. Its declaration still reserves rows when
// implicit inputs exist, even if no matrix read survives.
inline std::string unsupportedVaryingMatrixInput(const IRFunction& entry)
{
    bool matrix = false, implicit = false;
    for (const auto& p : entry.parameters) {
        matrix |= !p.varyingMatrixPaths.empty();
        implicit |= p.hasImplicitVaryingInput;
        if (p.hasImplicitVaryingMatrix)
            return "an implicit varying matrix needs row bindings";
    }
    if (!matrix) return {};
    if (implicit) return "a varying matrix beside an implicit input needs row reservations";

    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& inst : block->instructions) {
            if (!inst) continue;
            if (inst->op == IROp::LoadAttribute && inst->resultType.isMatrix())
                return "a varying matrix read needs row loads";
            for (const auto& p : entry.parameters) {
                for (const auto& path : p.varyingMatrixPaths) {
                    if (path.empty()) {
                        if (std::find(inst->operands.begin(), inst->operands.end(), p.valueId) != inst->operands.end())
                            return "a varying matrix read needs row loads";
                    } else if (inst->op == IROp::LoadAttribute && inst->structParamName == p.name &&
                               (inst->fieldName == path || inst->fieldName.compare(0, path.size() + 1, path + "[") == 0)) {
                        return "a varying matrix member read needs row loads";
                    }
                }
            }
        }
    }
    return {};
}

inline void bindImplicitAttributes(IRFunction& entry)
{
    const auto isInput = [](const IRParameter& p) {
        return p.storage == StorageQualifier::In || p.storage == StorageQualifier::None;
    };

    // What explicit semantics claim, on every input, read or not.
    std::set<int> claimed;
    const auto claim = [&](const std::string& sem, int index) {
        const int a = vpInputAttrIndex(upperSemantic(sem), index);
        if (a >= 0) claimed.insert(a);
    };
    for (const auto& p : entry.parameters) {
        if (!isInput(p) || isImplicitVertexParam(p)) continue;
        if (p.type.baseType == IRType::Void) {
            for (const auto& m : p.inputMembers)
                if (!m.uniform && !m.inferredSemantic && !m.semanticName.empty())
                    claim(m.semanticName, m.semanticIndex);
            continue;
        }
        if (p.semanticName.empty()) continue;
        const std::string sem = upperSemantic(p.semanticName);
        const bool repeated = sem == "POSITION" || sem == "NORMAL" || sem == "TANGENT" ||
                              sem == "BINORMAL" || sem == "DIFFUSE" || sem == "SPECULAR";
        const int count = p.type.isArray() && !repeated ? std::max(1, p.type.arraySize) : 1;
        for (int k = 0; k < count; ++k) claim(p.semanticName, p.semanticIndex + k);
    }

    // Which implicit inputs survive: a member by its load's qualified name,
    // a bare parameter by any remaining use of its value.
    std::set<std::string> read;
    std::set<IRValueID> used;
    for (const auto& block : entry.blocks) {
        if (!block) continue;
        for (const auto& inst : block->instructions) {
            if (!inst) continue;
            for (IRValueID op : inst->operands) used.insert(op);
            if (inst->op == IROp::LoadAttribute && inst->semanticName.empty() &&
                !inst->structParamName.empty())
                read.insert(inst->structParamName + "." + inst->fieldName);
        }
    }

    // Lowest free index, declaration order.
    std::map<std::string, int> index;
    int next = 0;
    const auto take = [&](const std::string& name) {
        if (!read.count(name) || index.count(name)) return;
        while (claimed.count(next)) ++next;
        index[name] = next++;
    };
    for (const auto& p : entry.parameters) {
        if (!isInput(p)) continue;
        if (p.type.baseType == IRType::Void) {
            for (const auto& m : p.inputMembers)
                if (!m.uniform && m.inferredSemantic && m.semanticName.empty())
                    take(p.name + "." + m.path);
        } else if (isImplicitVertexParam(p)) {
            if (used.count(p.valueId)) read.insert(p.name);
            take(p.name);
        }
    }

    for (auto& block : entry.blocks) {
        if (!block) continue;
        for (auto& inst : block->instructions) {
            if (!inst || inst->op != IROp::LoadAttribute || !inst->semanticName.empty() ||
                inst->structParamName.empty())
                continue;
            const auto it = index.find(inst->structParamName + "." + inst->fieldName);
            if (it != index.end()) inst->semanticIndex = it->second;
        }
    }
    for (auto& p : entry.parameters) {
        if (!isImplicitVertexParam(p)) continue;
        const auto it = index.find(p.name);
        if (it != index.end()) {
            p.semanticIndex = it->second;
            p.rawSemanticName = "ATTR" + std::to_string(it->second);
        } else {
            // Unread: no attribute; the container records it unassigned.
            p.semanticName.clear();
            p.rawSemanticName.clear();
        }
    }
}

} // namespace rsx_cg
