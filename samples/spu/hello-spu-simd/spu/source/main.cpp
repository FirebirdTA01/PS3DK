// hello-spu-simd — SPU side.
//
// Drives the simd::* class wrappers defined in <simd>: builds a few
// vectors of each major type, runs arithmetic / compare / select / shift
// / reduction operations, then packs every result into the 16-byte
// aligned ppu_results struct that the PPU passed in r3 (arg1).
//
// The PPU side knows the layout of that struct and prints each field.

#include <stdint.h>
#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_thread.h>

#include <simd>

using namespace simd;

// Mirror of the PPU-side struct.  Keep aligned(16) and total size a
// multiple of 16 — DMA put requires both.
struct alignas(16) Results {
    float  flt_lanes[4];   // float4 fma test    (a*b + b)
    int    int_lanes[4];   // int4 div+shift     ((ia/2) << 1)
    uint32_t bool_gather;   // bool4 gather mask
    uint32_t bool_any;      // any(lt) ? 1 : 0
    uint32_t bool_all;      // all(lt) ? 1 : 0
    uint32_t pad0;
    double dbl_lanes[2];    // double2 mul        (da * da)
    int64_t llong_lanes[2]; // llong2 add carry   (la + 0x100000000ll)
    uint64_t ullong_lanes[2]; // ullong2 64-bit subtract
    uint16_t short_lanes[8]; // short8 add
    uint16_t ushort_pack;    // gather of bool8 mask
    uint16_t pad1[7];
    uint8_t  uchar_lanes[16];// uchar16 add
};

extern "C" int main(uint64_t arg1, uint64_t arg2, uint64_t arg3, uint64_t arg4)
{
    (void)arg2; (void)arg3; (void)arg4;
    uint64_t out_ea = arg1;

    alignas(16) Results r{};

    // float4: fused-multiply-add and lane extraction.
    {
        float4 a(1.0f, 2.0f, 3.0f, 4.0f);
        float4 b(0.5f);
        float4 v = a * b + b;
        r.flt_lanes[0] = v[0];
        r.flt_lanes[1] = v[1];
        r.flt_lanes[2] = v[2];
        r.flt_lanes[3] = v[3];
    }

    // int4: divide by splat then left-shift; exercises scalar fallback
    // div + spu_sl based shift.
    {
        int4 ia(10, 20, 30, 40);
        int4 quot = ia / int4(2);
        quot <<= uint4(1u);
        r.int_lanes[0] = quot[0];
        r.int_lanes[1] = quot[1];
        r.int_lanes[2] = quot[2];
        r.int_lanes[3] = quot[3];
    }

    // bool4: build a < b mask and reduce.
    {
        float4 a(1.0f, 2.0f, 3.0f, 4.0f);
        float4 b(2.5f);
        bool4 lt = a < b;       // {true, true, false, false}
        r.bool_gather = (uint32_t)gather(lt);
        r.bool_any    = any(lt) ? 1u : 0u;
        r.bool_all    = all(lt) ? 1u : 0u;
    }

    // double2: multiply.
    {
        double2 d(1.5, 2.5);
        double2 m = d * d;       // {2.25, 6.25}
        r.dbl_lanes[0] = m[0];
        r.dbl_lanes[1] = m[1];
    }

    // llong2: add — exercises the 32-bit-halves + carry-propagate path.
    {
        llong2 la(0x1ll, -1ll);                 // -1 = 0xffff...
        llong2 lb = la + llong2(0x100000000ll); // {0x100000001, 0xffffffff}
        r.llong_lanes[0] = (int64_t)lb[0];
        r.llong_lanes[1] = (int64_t)lb[1];
    }

    // ullong2: subtract — exercises the borrow path.
    {
        ullong2 ua(0x100000000ull, 0x200000000ull);
        ullong2 ub(0x1ull, 0x1ull);
        ullong2 ud = ua - ub;                  // {0xffffffff, 0x1ffffffff}
        r.ullong_lanes[0] = (uint64_t)ud[0];
        r.ullong_lanes[1] = (uint64_t)ud[1];
    }

    // short8: add splatting all lanes.
    {
        short8 sa(100, 200, 300, 400, 500, 600, 700, 800);
        short8 sb = sa + short8((short)11);
        for (int i = 0; i < 8; ++i)
            r.short_lanes[i] = (uint16_t)sb[i];
    }

    // bool8: build a comparison and gather to verify mask packing.
    {
        ushort8 ua(0, 5, 10, 15, 20, 25, 30, 35);
        bool8 mask = ua > ushort8((ushort)17);  // {f,f,f,f,t,t,t,t}
        r.ushort_pack = (uint16_t)gather(mask);
    }

    // uchar16: add and lane store.
    {
        uchar16 ca((uchar)1, 2, 3, 4, 5, 6, 7, 8,
                   9, 10, 11, 12, 13, 14, 15, 16);
        uchar16 cb = ca + uchar16((uchar)100);
        for (int i = 0; i < 16; ++i)
            r.uchar_lanes[i] = (uint8_t)cb[i];
    }

    // DMA results back to the PPU-allocated buffer at out_ea.
    mfc_put(&r, out_ea, sizeof(r), 0, 0, 0);
    mfc_write_tag_mask(1u << 0);
    mfc_read_tag_status_all();

    sys_spu_thread_exit(0);
    return 0;
}
