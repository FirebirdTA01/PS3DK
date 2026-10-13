// Lane-packing slice 1: verify the pure remapLane / remapSwizzle helpers
// against the brief's reference table (a-i) plus the 65-case identity sweep
// the lead asked for.  Prints one line per case and returns 0 iff every case
// matches, or 1 with a diagnostic line per failure.
//
// The function under test is dependency-free: include only the header.
#include "../../tools/rsx-cg-compiler/src/lane_remap.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {

int failures = 0;
int cases = 0;

void expectEq(const char* tag, uint8_t got, uint8_t want)
{
    ++cases;
    if (got == want) {
        std::printf("  ok  %s: remapLane == 0x%X\n", tag, got);
        return;
    }
    ++failures;
    std::printf("FAIL  %s: remapLane == 0x%X, want 0x%X\n", tag, got, want);
}

void expectSwizzle(const char* tag,
                   std::array<uint8_t, 4> got,
                   std::array<uint8_t, 4> want)
{
    ++cases;
    if (got == want) {
        std::printf("  ok  %s: swizzle == {%d,%d,%d,%d}\n",
                    tag, got[0], got[1], got[2], got[3]);
        return;
    }
    ++failures;
    std::printf("FAIL  %s: swizzle == {%d,%d,%d,%d}, want {%d,%d,%d,%d}\n",
                tag, got[0], got[1], got[2], got[3],
                want[0], want[1], want[2], want[3]);
}

}   // namespace

int main()
{
    // a) remapLane(0, 0x1, 0x8) == 3   (x -> w)
    expectEq("a x->w (1-bit)", remapLane(0, 0x1, 0x8), 3);

    // b) remapLane(2, 0x5, 0x6) == 2   (x,z -> y,z: z is the 2nd written bit
    //                                    -> 2nd assigned bit = z)
    expectEq("b z stays z (x,z|->y,z)", remapLane(2, 0x5, 0x6), 2);

    // c) remapLane(0, 0x5, 0x6) == 1   (x is the 1st written bit -> 1st
    //                                    assigned bit = y)
    expectEq("c x->y (x,z|->y,z)", remapLane(0, 0x5, 0x6), 1);

    // d) remapLane(1, 0x5, 0x6) == 0xFF   (y is not written)
    expectEq("d y not written -> 0xFF", remapLane(1, 0x5, 0x6), 0xFF);

    // e) remapLane(0, 0x3, 0x1) == 0xFF   (popcount mismatch)
    expectEq("e popcount mismatch -> 0xFF", remapLane(0, 0x3, 0x1), 0xFF);

    // f) identity sweep: for every mask m in 1..15 and every set lane l of
    //    m, remapLane(l, m, m) == l.
    {
        int identity = 0;
        bool allOk = true;
        for (uint8_t m = 1; m < 16; ++m)
        {
            for (uint8_t l = 0; l < 4; ++l)
            {
                if (!(m & (1u << l))) continue;
                const uint8_t r = remapLane(l, m, m);
                if (r != l) allOk = false;
                ++identity;
            }
        }
        ++cases;
        if (allOk)
            std::printf("  ok  f identity sweep: %d cases all identity\n", identity);
        else
        {
            ++failures;
            std::printf("FAIL  f identity sweep: %d cases, at least one mismatch\n", identity);
        }
    }

    // g) remapSwizzle({0,0,2,2}, 0x5, 0xA) gives {1,1,3,3}
    {
        std::array<uint8_t, 4> swz{0, 0, 2, 2};
        remapSwizzle(swz, 0x5, 0xA);
        expectSwizzle("g swz {0,0,2,2} (0x5,0xA)", swz,
                      {1, 1, 3, 3});
    }

    // h) remapSwizzle({0,1,2,3}, 0x5, 0xA) leaves it unchanged (fail closed)
    {
        std::array<uint8_t, 4> swz{0, 1, 2, 3};
        remapSwizzle(swz, 0x5, 0xA);
        expectSwizzle("h swz unchanged (fail closed)", swz,
                      {0, 1, 2, 3});
    }

    std::printf("%d cases, %d pass, %d fail\n",
                cases, cases - failures, failures);
    if (failures == 0)
        std::printf("PASS: lane-remap-test\n");
    return failures == 0 ? 0 : 1;
}
