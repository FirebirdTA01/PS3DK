/* spu-lowpart-spill (SPU side): narrow a value that was spilled to the
 * stack, in the widths SPU has, and check the bytes that come back.
 *
 * SPU loads and stores move a whole quadword and keep a value narrower
 * than 16 bytes in the register's preferred slot, so a narrower load at a
 * big-endian lowpart offset does not read the low part of a wider value.
 * Each case makes its value opaque (an asm that may rewrite it, so the
 * compiler cannot rebuild it from its inputs afterwards) and then keeps it
 * live across an asm that clobbers every allocatable register, so the
 * value has to go through a stack slot; then it returns a narrowing of it.
 * Which mode the slot really has is a property of the compiled code, not of
 * the C type: the test driver reads it from the reload dump.  All expected
 * values are written out as literals.
 *
 * argument 1: effective address of a PPU u64 holding V64 (the __ea case).
 * Exits with 0 when every case matches, else with a bitmask of the failing
 * cases (bit n = case n); the PPU side prints the names.
 */
#include <stdint.h>
#include <spu_intrinsics.h>
#include <sys/spu_thread.h>

/* Every register a value could stay in across the asm: $2-$125 and $127.
 * $0 (link register) and $1 (stack pointer) are fixed.  $126 is the PIC
 * register, which no asm may clobber, so this file is built with
 * -ffixed-126 (CMakeLists.txt) to keep values out of it. */
#define SPILL_ALL() __asm__ volatile ("" ::: "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$16", "$17", "$18", "$19", "$20", "$21", "$22", "$23", "$24", "$25", "$26", "$27", "$28", "$29", "$30", "$31", "$32", "$33", "$34", "$35", "$36", "$37", "$38", "$39", "$40", "$41", "$42", "$43", "$44", "$45", "$46", "$47", "$48", "$49", "$50", "$51", "$52", "$53", "$54", "$55", "$56", "$57", "$58", "$59", "$60", "$61", "$62", "$63", "$64", "$65", "$66", "$67", "$68", "$69", "$70", "$71", "$72", "$73", "$74", "$75", "$76", "$77", "$78", "$79", "$80", "$81", "$82", "$83", "$84", "$85", "$86", "$87", "$88", "$89", "$90", "$91", "$92", "$93", "$94", "$95", "$96", "$97", "$98", "$99", "$100", "$101", "$102", "$103", "$104", "$105", "$106", "$107", "$108", "$109", "$110", "$111", "$112", "$113", "$114", "$115", "$116", "$117", "$118", "$119", "$120", "$121", "$122", "$123", "$124", "$125", "$127")

/* The value must exist as one register value of its own type here: the asm
 * may have changed it, so nothing after it can be recomputed from the
 * inputs.  Then it has to survive the clobber in a stack slot. */
#define KEEP(v) do { __asm__ volatile ("" : "+r"(v)); SPILL_ALL(); } while (0)

#define NOINLINE __attribute__((noinline, noclone))

/* Inputs come from volatile globals so nothing folds at compile time.
 * Every byte of each value is distinct and the sign bits are set. */
volatile uint64_t in64 = 0x8192a3b4c5d6e7f8ull;
volatile uint32_t in32 = 0x89abcdefu;
volatile uint16_t in16 = 0x8a9bu;
volatile uint64_t in128_lo = 0x0718293a4b5c6d7eull;
volatile float inf32 = 1.5f;               /* bits 0x3fc00000 */
volatile double inf64 = -2.25;             /* bits 0xc002000000000000 */
volatile double inf64_cvt = 0.15625;       /* (float) bits 0x3e200000 */

NOINLINE int32_t di_si_s(void) { uint64_t v = in64; KEEP(v); return (int32_t)v; }
NOINLINE uint32_t di_si_u(void) { uint64_t v = in64; KEEP(v); return (uint32_t)v; }
NOINLINE int16_t di_hi_s(void) { uint64_t v = in64; KEEP(v); return (int16_t)v; }
NOINLINE uint16_t di_hi_u(void) { uint64_t v = in64; KEEP(v); return (uint16_t)v; }
NOINLINE int8_t di_qi_s(void) { uint64_t v = in64; KEEP(v); return (int8_t)v; }
NOINLINE uint8_t di_qi_u(void) { uint64_t v = in64; KEEP(v); return (uint8_t)v; }
NOINLINE int16_t si_hi_s(void) { uint32_t v = in32; KEEP(v); return (int16_t)v; }
NOINLINE uint16_t si_hi_u(void) { uint32_t v = in32; KEEP(v); return (uint16_t)v; }
NOINLINE int8_t si_qi_s(void) { uint32_t v = in32; KEEP(v); return (int8_t)v; }
NOINLINE uint8_t si_qi_u(void) { uint32_t v = in32; KEEP(v); return (uint8_t)v; }
NOINLINE int8_t hi_qi_s(void) { uint16_t v = in16; KEEP(v); return (int8_t)v; }
NOINLINE uint8_t hi_qi_u(void) { uint16_t v = in16; KEEP(v); return (uint8_t)v; }

NOINLINE uint64_t ti_di(void)
{
    unsigned __int128 v = ((unsigned __int128)in64 << 64) | in128_lo;
    KEEP(v);
    return (uint64_t)v;
}
NOINLINE uint32_t ti_si(void)
{
    unsigned __int128 v = ((unsigned __int128)in64 << 64) | in128_lo;
    KEEP(v);
    return (uint32_t)v;
}
NOINLINE uint8_t ti_qi(void)
{
    unsigned __int128 v = ((unsigned __int128)in64 << 64) | in128_lo;
    KEEP(v);
    return (uint8_t)v;
}

NOINLINE uint64_t v2di_e0(void)
{
    vec_ullong2 v = spu_insert(in128_lo, spu_promote(in64, 0), 1);
    KEEP(v);
    return spu_extract(v, 0);
}
NOINLINE uint64_t v2di_e1(void)
{
    vec_ullong2 v = spu_insert(in128_lo, spu_promote(in64, 0), 1);
    KEEP(v);
    return spu_extract(v, 1);
}
NOINLINE uint32_t v4si_e3(void)
{
    vec_uint4 v = spu_insert(in32, spu_splats((uint32_t)0x11223344u), 3);
    KEEP(v);
    return spu_extract(v, 3);
}

NOINLINE uint32_t sf_keep(void)
{
    union { float f; uint32_t u; } r;
    float f = inf32;
    KEEP(f);
    r.f = f;
    return r.u;
}
NOINLINE uint64_t df_keep(void)
{
    union { double d; uint64_t u; } r;
    double d = inf64;
    KEEP(d);
    r.d = d;
    return r.u;
}
NOINLINE uint32_t df_to_sf(void)
{
    union { float f; uint32_t u; } r;
    double d = inf64_cvt;
    KEEP(d);
    r.f = (float)d;
    return r.u;
}
/* Control: the high word, which the bad fold returned for the low word. */
NOINLINE int32_t di_hiword(void) { uint64_t v = in64; KEEP(v); return (int32_t)(v >> 32); }

NOINLINE int32_t ea_di_si(__ea uint64_t *p) { uint64_t v = *p; KEEP(v); return (int32_t)v; }

/* An incoming stack argument: SPU passes the first 72 arguments in
 * registers ($3-$74), so a75 arrives in its own 16-byte stack slot, which
 * can serve as the memory equivalent of its pseudo.  It is used once before
 * the clobber (so it is live across it, and its pseudo cannot be given a
 * register there), then narrowed: the reload of (subreg:QI/HI (reg:SI)) of a
 * pseudo with a memory equivalent.  Whether reload really takes that path is
 * read from the reload dump by the test driver, not assumed. */
NOINLINE uint8_t stackarg_si_qi(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13, uint32_t a14, uint32_t a15, uint32_t a16, uint32_t a17, uint32_t a18, uint32_t a19, uint32_t a20, uint32_t a21, uint32_t a22, uint32_t a23, uint32_t a24, uint32_t a25, uint32_t a26, uint32_t a27, uint32_t a28, uint32_t a29, uint32_t a30, uint32_t a31, uint32_t a32, uint32_t a33, uint32_t a34, uint32_t a35, uint32_t a36, uint32_t a37, uint32_t a38, uint32_t a39, uint32_t a40, uint32_t a41, uint32_t a42, uint32_t a43, uint32_t a44, uint32_t a45, uint32_t a46, uint32_t a47, uint32_t a48, uint32_t a49, uint32_t a50, uint32_t a51, uint32_t a52, uint32_t a53, uint32_t a54, uint32_t a55, uint32_t a56, uint32_t a57, uint32_t a58, uint32_t a59, uint32_t a60, uint32_t a61, uint32_t a62, uint32_t a63, uint32_t a64, uint32_t a65, uint32_t a66, uint32_t a67, uint32_t a68, uint32_t a69, uint32_t a70, uint32_t a71, uint32_t a72, uint32_t a73, uint32_t a74, uint32_t a75, uint32_t a76, uint32_t a77, uint32_t a78, uint32_t a79)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11; (void)a12; (void)a13; (void)a14; (void)a15; (void)a16; (void)a17; (void)a18; (void)a19; (void)a20; (void)a21; (void)a22; (void)a23; (void)a24; (void)a25; (void)a26; (void)a27; (void)a28; (void)a29; (void)a30; (void)a31; (void)a32; (void)a33; (void)a34; (void)a35; (void)a36; (void)a37; (void)a38; (void)a39; (void)a40; (void)a41; (void)a42; (void)a43; (void)a44; (void)a45; (void)a46; (void)a47; (void)a48; (void)a49; (void)a50; (void)a51; (void)a52; (void)a53; (void)a54; (void)a55; (void)a56; (void)a57; (void)a58; (void)a59; (void)a60; (void)a61; (void)a62; (void)a63; (void)a64; (void)a65; (void)a66; (void)a67; (void)a68; (void)a69; (void)a70; (void)a71; (void)a72; (void)a73; (void)a74; (void)a76; (void)a77; (void)a78; (void)a79;
    uint32_t v = a75;
    __asm__ volatile ("" :: "r"(v));
    SPILL_ALL();
    return (uint8_t)v;
}
NOINLINE uint16_t stackarg_si_hi(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13, uint32_t a14, uint32_t a15, uint32_t a16, uint32_t a17, uint32_t a18, uint32_t a19, uint32_t a20, uint32_t a21, uint32_t a22, uint32_t a23, uint32_t a24, uint32_t a25, uint32_t a26, uint32_t a27, uint32_t a28, uint32_t a29, uint32_t a30, uint32_t a31, uint32_t a32, uint32_t a33, uint32_t a34, uint32_t a35, uint32_t a36, uint32_t a37, uint32_t a38, uint32_t a39, uint32_t a40, uint32_t a41, uint32_t a42, uint32_t a43, uint32_t a44, uint32_t a45, uint32_t a46, uint32_t a47, uint32_t a48, uint32_t a49, uint32_t a50, uint32_t a51, uint32_t a52, uint32_t a53, uint32_t a54, uint32_t a55, uint32_t a56, uint32_t a57, uint32_t a58, uint32_t a59, uint32_t a60, uint32_t a61, uint32_t a62, uint32_t a63, uint32_t a64, uint32_t a65, uint32_t a66, uint32_t a67, uint32_t a68, uint32_t a69, uint32_t a70, uint32_t a71, uint32_t a72, uint32_t a73, uint32_t a74, uint32_t a75, uint32_t a76, uint32_t a77, uint32_t a78, uint32_t a79)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11; (void)a12; (void)a13; (void)a14; (void)a15; (void)a16; (void)a17; (void)a18; (void)a19; (void)a20; (void)a21; (void)a22; (void)a23; (void)a24; (void)a25; (void)a26; (void)a27; (void)a28; (void)a29; (void)a30; (void)a31; (void)a32; (void)a33; (void)a34; (void)a35; (void)a36; (void)a37; (void)a38; (void)a39; (void)a40; (void)a41; (void)a42; (void)a43; (void)a44; (void)a45; (void)a46; (void)a47; (void)a48; (void)a49; (void)a50; (void)a51; (void)a52; (void)a53; (void)a54; (void)a55; (void)a56; (void)a57; (void)a58; (void)a59; (void)a60; (void)a61; (void)a62; (void)a63; (void)a64; (void)a65; (void)a66; (void)a67; (void)a68; (void)a69; (void)a70; (void)a71; (void)a72; (void)a73; (void)a74; (void)a76; (void)a77; (void)a78; (void)a79;
    uint32_t v = a75;
    __asm__ volatile ("" :: "r"(v));
    SPILL_ALL();
    return (uint16_t)v;
}

/* A vector stack argument (position 75, so not in $3-$74): its pseudo is
 * loaded from its 16-byte slot, which can be its memory equivalent; after the
 * clobber it is read as another 16-byte vector type, a same-size SUBREG,
 * which is the SUBREG form SPU's can_change_mode_class lets reload take
 * through the mode-dependent-address branch.  The driver checks the reload
 * dump for what reload really did. */
NOINLINE uint32_t stackarg_v2di_v4si(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7, uint32_t a8, uint32_t a9, uint32_t a10, uint32_t a11, uint32_t a12, uint32_t a13, uint32_t a14, uint32_t a15, uint32_t a16, uint32_t a17, uint32_t a18, uint32_t a19, uint32_t a20, uint32_t a21, uint32_t a22, uint32_t a23, uint32_t a24, uint32_t a25, uint32_t a26, uint32_t a27, uint32_t a28, uint32_t a29, uint32_t a30, uint32_t a31, uint32_t a32, uint32_t a33, uint32_t a34, uint32_t a35, uint32_t a36, uint32_t a37, uint32_t a38, uint32_t a39, uint32_t a40, uint32_t a41, uint32_t a42, uint32_t a43, uint32_t a44, uint32_t a45, uint32_t a46, uint32_t a47, uint32_t a48, uint32_t a49, uint32_t a50, uint32_t a51, uint32_t a52, uint32_t a53, uint32_t a54, uint32_t a55, uint32_t a56, uint32_t a57, uint32_t a58, uint32_t a59, uint32_t a60, uint32_t a61, uint32_t a62, uint32_t a63, uint32_t a64, uint32_t a65, uint32_t a66, uint32_t a67, uint32_t a68, uint32_t a69, uint32_t a70, uint32_t a71, uint32_t a72, uint32_t a73, uint32_t a74, vec_ullong2 a75)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6; (void)a7; (void)a8; (void)a9; (void)a10; (void)a11; (void)a12; (void)a13; (void)a14; (void)a15; (void)a16; (void)a17; (void)a18; (void)a19; (void)a20; (void)a21; (void)a22; (void)a23; (void)a24; (void)a25; (void)a26; (void)a27; (void)a28; (void)a29; (void)a30; (void)a31; (void)a32; (void)a33; (void)a34; (void)a35; (void)a36; (void)a37; (void)a38; (void)a39; (void)a40; (void)a41; (void)a42; (void)a43; (void)a44; (void)a45; (void)a46; (void)a47; (void)a48; (void)a49; (void)a50; (void)a51; (void)a52; (void)a53; (void)a54; (void)a55; (void)a56; (void)a57; (void)a58; (void)a59; (void)a60; (void)a61; (void)a62; (void)a63; (void)a64; (void)a65; (void)a66; (void)a67; (void)a68; (void)a69; (void)a70; (void)a71; (void)a72; (void)a73; (void)a74;
    vec_ullong2 v = a75;
    __asm__ volatile ("" :: "r"(v));
    SPILL_ALL();
    vec_uint4 w = (vec_uint4)v;
    return spu_extract(w, 3);
}

int main(uint64_t ea_v64)
{
    int mask = 0;
    __ea uint64_t *p = (__ea uint64_t *)(uint32_t)ea_v64;

    if (di_si_s() != (int32_t)0xc5d6e7f8u) mask |= 1 << 0;
    if (di_si_u() != 0xc5d6e7f8u) mask |= 1 << 1;
    if (di_hi_s() != (int16_t)0xe7f8u) mask |= 1 << 2;
    if (di_hi_u() != 0xe7f8u) mask |= 1 << 3;
    if (di_qi_s() != (int8_t)0xf8u) mask |= 1 << 4;
    if (di_qi_u() != 0xf8u) mask |= 1 << 5;
    if (si_hi_s() != (int16_t)0xcdefu) mask |= 1 << 6;
    if (si_hi_u() != 0xcdefu) mask |= 1 << 7;
    if (si_qi_s() != (int8_t)0xefu) mask |= 1 << 8;
    if (si_qi_u() != 0xefu) mask |= 1 << 9;
    if (hi_qi_s() != (int8_t)0x9bu) mask |= 1 << 10;
    if (hi_qi_u() != 0x9bu) mask |= 1 << 11;
    if (ti_di() != 0x0718293a4b5c6d7eull) mask |= 1 << 12;
    if (ti_si() != 0x4b5c6d7eu) mask |= 1 << 13;
    if (ti_qi() != 0x7eu) mask |= 1 << 14;
    if (v2di_e0() != 0x8192a3b4c5d6e7f8ull) mask |= 1 << 15;
    if (v2di_e1() != 0x0718293a4b5c6d7eull) mask |= 1 << 16;
    if (v4si_e3() != 0x89abcdefu) mask |= 1 << 17;
    if (sf_keep() != 0x3fc00000u) mask |= 1 << 18;
    if (df_keep() != 0xc002000000000000ull) mask |= 1 << 19;
    if (df_to_sf() != 0x3e200000u) mask |= 1 << 20;
    if (di_hiword() != (int32_t)0x8192a3b4u) mask |= 1 << 21;
    if (ea_di_si(p) != (int32_t)0xc5d6e7f8u) mask |= 1 << 22;
    if (stackarg_si_qi(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, in32, 0, 0, 0, 0) != 0xefu) mask |= 1 << 23;
    if (stackarg_si_hi(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, in32, 0, 0, 0, 0) != 0xcdefu) mask |= 1 << 24;
    if (stackarg_v2di_v4si(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, spu_insert(in128_lo, spu_promote(in64, 0), 1)) != 0x4b5c6d7eu) mask |= 1 << 25;

    sys_spu_thread_exit(mask);
    return 0;
}
