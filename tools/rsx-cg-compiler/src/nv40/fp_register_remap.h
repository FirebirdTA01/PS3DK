#pragma once
#include <map>
#include <optional>
#include <set>

namespace nv40::detail {
// Whole R slots, including both aliased H halves. Fixed slots are exports.
inline std::optional<std::map<int, int>> planFpRegisterRemap(
    const std::set<int>& used, const std::set<int>& half,
    const std::set<int>& fixed)
{
    // The existing guards allow raw indices 0..47 and fewer than48 R
    // slots. Half indices 0..47 occupy whole slots 0..23.
    if (used.size() > 47) return std::nullopt;
    std::map<int, int> result;
    std::set<int> occupied;
    for (int slot : used) if (slot < 0) return std::nullopt;
    for (int slot : half) if (!used.count(slot)) return std::nullopt;
    for (int slot : fixed) {
        if (!used.count(slot) || slot >= 47 || slot < 0 ||
            (half.count(slot) && slot >= 24)) return std::nullopt;
        result[slot] = slot;
        occupied.insert(slot);
    }
    // Reserve all half-used slots before full-only slots. Keep an old
    // number whenever it fits; move the displaced full slots afterwards.
    for (bool halves : {true, false}) {
        const int limit = halves ? 24 : 47;
        for (int slot : used) {
            if (bool(half.count(slot)) != halves || result.count(slot)) continue;
            if (slot < limit && !occupied.count(slot)) {
                result[slot] = slot;
                occupied.insert(slot);
            }
        }
        for (int slot : used) {
            if (bool(half.count(slot)) != halves || result.count(slot)) continue;
            int to = 0;
            while (to < limit && occupied.count(to)) ++to;
            if (to == limit) return std::nullopt;
            result[slot] = to;
            occupied.insert(to);
        }
    }
    return result;
}
}
