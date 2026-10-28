#pragma once
// One value id per distinct constant.  The frontend gives every literal
// its own IRConstant, so `1.0 / k.x` written twice was two divides that
// common-subexpression elimination could not merge: it compares operand
// ids, and the two `1.0`s had different ones (measured: the established compiler
// computes the reciprocal once).  Constants with the same type and the
// same bits (so 0.0 and -0.0, or two NaN payloads, stay apart) are
// rewritten to the lowest id among them.

#include "ir.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace rsx_cg {

inline std::string constantKey(const IRConstant& c)
{
    const IRTypeInfo& t = c.type;
    std::string key = std::to_string(static_cast<int>(t.baseType)) + ":" +
                      std::to_string(static_cast<int>(t.elementType)) + ":" +
                      std::to_string(t.vectorSize) + ":" + std::to_string(t.matrixRows) + ":" +
                      std::to_string(t.matrixCols) + ":" + std::to_string(t.arraySize) + "|" +
                      std::to_string(c.value.index()) + "|";
    const auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
    if (const auto* v = std::get_if<bool>(&c.value)) key += *v ? "1" : "0";
    else if (const auto* v = std::get_if<int32_t>(&c.value)) key += std::to_string(*v);
    else if (const auto* v = std::get_if<uint32_t>(&c.value)) key += std::to_string(*v);
    else if (const auto* v = std::get_if<float>(&c.value)) key += std::to_string(bits(*v));
    else if (const auto* v = std::get_if<std::vector<float>>(&c.value))
        for (float f : *v) key += std::to_string(bits(f)) + ",";
    key += "|";
    for (int64_t i : c.intValues) key += std::to_string(i) + ",";
    return key;
}

// Returns whether any operand was rewritten.
inline bool canonicalizeConstants(IRFunction& fn)
{
    std::map<IRValueID, const IRConstant*> constants;   // ordered: lowest id wins
    for (const auto& [id, value] : fn.values)
        if (const auto* c = dynamic_cast<const IRConstant*>(value.get())) constants[id] = c;
    std::map<std::string, IRValueID> first;
    std::map<IRValueID, IRValueID> canonical;
    for (const auto& [id, c] : constants) {
        const auto [it, inserted] = first.emplace(constantKey(*c), id);
        if (!inserted) canonical[id] = it->second;
    }
    if (canonical.empty()) return false;
    bool changed = false;
    for (auto& block : fn.blocks) {
        if (!block) continue;
        for (auto& inst : block->instructions) {
            if (!inst) continue;
            for (IRValueID& op : inst->operands) {
                const auto it = canonical.find(op);
                if (it != canonical.end()) { op = it->second; changed = true; }
            }
        }
    }
    return changed;
}

// One spelling per lane read.  `v.y` is a scalar VecShuffle of v, while the
// lowering of any(v) or of a lane of a sub-swizzle (`v.zw[0]`) produces a
// VecExtract with a constant index, possibly of a VecShuffle.  The emitter
// reads both the same way, but common-subexpression elimination compares
// opcodes and operand ids, so once fragment literals share one id the
// explicit spelling merged its comparisons and any(v) did not (any-reduction:
// 31 vs 40 instructions for the two forms).  A constant-index extract becomes
// a scalar shuffle, and a shuffle of a shuffle becomes one shuffle of the
// original vector.  Matrices are left alone (an extract there is a row).
// Returns whether any instruction was rewritten.
inline bool canonicalizeLaneReads(IRFunction& fn)
{
    std::map<IRValueID, IRInstruction*> defs;
    for (auto& block : fn.blocks) {
        if (!block) continue;
        for (auto& inst : block->instructions)
            if (inst && inst->result != InvalidIRValue) defs[inst->result] = inst.get();
    }
    const auto vectorOf = [&](IRValueID id) -> int {   // lanes of a plain vector value, 0 otherwise
        const IRTypeInfo* t = nullptr;
        if (const IRValue* v = fn.getValue(id)) t = &v->type;
        else if (const auto d = defs.find(id); d != defs.end()) t = &d->second->resultType;
        if (!t || t->matrixRows > 0 || t->matrixCols > 0 || t->arraySize > 0) return 0;
        return t->vectorSize;
    };
    const auto constantLane = [&](IRValueID id, int& out) -> bool {
        const auto* c = dynamic_cast<const IRConstant*>(fn.getValue(id));
        if (!c) return false;
        if (const auto* i = std::get_if<int32_t>(&c->value)) out = *i;
        else if (const auto* u = std::get_if<uint32_t>(&c->value)) out = static_cast<int>(*u);
        else if (const auto* f = std::get_if<float>(&c->value)) {
            if (static_cast<float>(static_cast<int>(*f)) != *f) return false;
            out = static_cast<int>(*f);
        } else return false;
        return true;
    };
    bool changed = false;
    for (auto& block : fn.blocks) {
        if (!block) continue;
        for (auto& inst : block->instructions) {
            if (!inst || inst->operands.empty()) continue;
            const int width = inst->resultType.vectorSize;
            if (inst->resultType.matrixRows > 0 || inst->resultType.matrixCols > 0 ||
                inst->resultType.arraySize > 0 || width < 1 || width > 4)
                continue;
            if (inst->op == IROp::VecExtract && width == 1) {
                const int lanes = vectorOf(inst->operands[0]);
                int lane = inst->componentIndex;
                if (lanes < 2 || (inst->operands.size() >= 2 && !constantLane(inst->operands[1], lane)) ||
                    lane < 0 || lane >= lanes)
                    continue;
                inst->op = IROp::VecShuffle;
                inst->operands.resize(1);
                inst->swizzleMask = lane;
                inst->componentIndex = 0;
                changed = true;
            }
            if (inst->op != IROp::VecShuffle || inst->operands.size() != 1) continue;
            const auto inner = defs.find(inst->operands[0]);
            if (inner == defs.end() || inner->second->op != IROp::VecShuffle ||
                inner->second->operands.size() != 1 || vectorOf(inner->second->operands[0]) < 2)
                continue;
            int mask = 0;
            for (int i = 0; i < width; ++i) {
                const int mid = (inst->swizzleMask >> (2 * i)) & 3;
                mask |= ((inner->second->swizzleMask >> (2 * mid)) & 3) << (2 * i);
            }
            inst->operands[0] = inner->second->operands[0];
            inst->swizzleMask = mask;
            changed = true;
        }
    }
    return changed;
}

} // namespace rsx_cg
