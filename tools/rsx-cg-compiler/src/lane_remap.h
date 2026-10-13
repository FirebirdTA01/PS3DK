// Lane-packing slice 1 (infrastructure): pure lane-remap helpers.  A vreg
// that WROTE a subset of lanes `written` was ASSIGNED the subset `assigned`;
// a reader asking for logical lane `lane` (a set bit of `written`) must read
// physical lane remapLane(lane, written, assigned).  The k-th set bit of
// `written` maps to the k-th set bit of `assigned`, so relative ordering is
// preserved.  Fail closed: any precondition violation returns 0xFF, and
// remapSwizzle leaves the swizzle untouched if any component fails.
//
// This header is dependency-free and inline; the lowering only includes it
// and reads from it.  Slice 1 does not yet call remapSwizzle from the lowering
// (the slot-sharing slice does), so the production output is byte-identical
// to the base; the map and these functions are verified by
// tests/shader-compiler/lane-remap-test.{cpp,sh}.
#pragma once
#include <array>
#include <cstdint>

inline uint8_t popcount4(uint8_t m)
{
    uint8_t n = 0;
    while (m) { n += uint8_t{1}; m &= m - 1; }
    return n;
}

inline uint8_t kthSetBit(uint8_t m, int k)
{
    for (int i = 0; i < 4 && k >= 0; ++i)
    {
        if (m & (1u << i))
        {
            if (k == 0) return static_cast<uint8_t>(i);
            --k;
        }
    }
    return 0xFF;
}

inline uint8_t remapLane(uint8_t lane, uint8_t written, uint8_t assigned)
{
    if (lane > 3) return 0xFF;
    if (!(written & (1u << lane))) return 0xFF;           // lane not written
    if (popcount4(written) != popcount4(assigned)) return 0xFF;
    // k = index of `lane` among the set bits of written
    int k = 0;
    for (int i = 0; i < lane; ++i)
        if (written & (1u << i)) ++k;
    return kthSetBit(assigned, k);
}

inline void remapSwizzle(std::array<uint8_t, 4>& swz, uint8_t written, uint8_t assigned)
{
    if (popcount4(written) != popcount4(assigned)) return;   // fail closed
    std::array<uint8_t, 4> out;
    bool ok = true;
    for (int i = 0; i < 4; ++i)
    {
        const uint8_t r = remapLane(swz[i], written, assigned);
        if (r == 0xFF) { ok = false; break; }
        out[i] = r;
    }
    if (ok) swz = out;
}
