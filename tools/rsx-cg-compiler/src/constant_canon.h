#pragma once
// One value id per distinct constant.  The frontend gives every literal
// its own IRConstant, so `1.0 / k.x` written twice was two divides that
// common-subexpression elimination could not merge: it compares operand
// ids, and the two `1.0`s had different ones (measured: the reference
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

} // namespace rsx_cg
