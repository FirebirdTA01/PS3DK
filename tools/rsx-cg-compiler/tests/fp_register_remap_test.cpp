#include "nv40/fp_register_remap.h"
#include <iostream>
#include <stdexcept>
using namespace nv40::detail;
static void require(bool ok) { if (!ok) throw std::runtime_error("register remap invariant failed"); }
static void check(const std::set<int>& used, const std::set<int>& half,
                  const std::set<int>& fixed, bool fits) {
    const auto plan = planFpRegisterRemap(used, half, fixed);
    require(bool(plan) == fits);
    if (!plan) return;
    std::set<int> destinations;
    for (int old : used) {
        const int to = plan->at(old);
        require(to >= 0 && to < 47);
        require(destinations.insert(to).second);
        if (half.count(old)) require(to < 24);
        if (fixed.count(old)) require(to == old);
        // Both H halves still alias their original full register slot.
        for (int parity : {0, 1}) {
            const int h = (to << 1) | parity;
            require((h >> 1) == to && (h & 1) == parity);
        }
    }
    require(plan->size() == used.size());
}
int main() {
    try {
        std::set<int> slots;
        for (int i=0;i<25;++i) slots.insert(i);
        check(slots, {24}, {0}, true); // H48 in only25 R slots
        check(slots, {0,24}, {0,1,2,3,4}, true); // half colour, depth, MRT
        check(slots, slots, {0}, false); //25 distinct half-used slots cannot fit
        check({0,1,62}, {62}, {0,1}, true); // sparse numbering
        check({0,24}, {24}, {24}, false); // cannot move a fixed half export
        check({0,48}, {}, {48}, false);
        slots.clear();
        for(int i=0;i<47;++i) slots.insert(i);
        check(slots, {40,41,42}, {0,1,2,3,4}, true);
        slots.insert(47);
        check(slots, {}, {0}, false);
        // Every distribution of half/full slots in a small alias graph.
        for(int mask=0;mask<256;++mask) {
            std::set<int> used{0}, half;
            for(int i=0;i<8;++i) { int s=20+i; used.insert(s); if(mask&(1<<i)) half.insert(s); }
            check(used,half,{0},true);
        }
        std::cout << "fp-register-remap: 264 cases passed\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
