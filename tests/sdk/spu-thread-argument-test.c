/*
 * tests/sdk/spu-thread-argument-test.c
 *
 * Verifies canonical sys_spu_thread_argument_t layout (arg1..arg4 at offsets 0, 8, 16, 24),
 * legacy sysSpuThreadArgument compatibility (arg0..arg3), initializer zeroing, and
 * call forwarding to sysSpuThreadInitialize.
 */

#include <stdint.h>
#include <stddef.h>
#include <sys/spu_thread_group.h>
#include <sys/spu_thread.h>

#if defined(__cplusplus)
static_assert(sizeof(sys_spu_thread_argument_t) == 32, "sizeof sys_spu_thread_argument_t must be 32");
static_assert(offsetof(sys_spu_thread_argument_t, arg1) == 0, "offset of arg1 must be 0");
static_assert(offsetof(sys_spu_thread_argument_t, arg2) == 8, "offset of arg2 must be 8");
static_assert(offsetof(sys_spu_thread_argument_t, arg3) == 16, "offset of arg3 must be 16");
static_assert(offsetof(sys_spu_thread_argument_t, arg4) == 24, "offset of arg4 must be 24");

static_assert(sizeof(sysSpuThreadArgument) == 32, "sizeof sysSpuThreadArgument must be 32");
static_assert(offsetof(sysSpuThreadArgument, arg0) == 0, "offset of arg0 must be 0");
static_assert(offsetof(sysSpuThreadArgument, arg1) == 8, "offset of arg1 must be 8");
static_assert(offsetof(sysSpuThreadArgument, arg2) == 16, "offset of arg2 must be 16");
static_assert(offsetof(sysSpuThreadArgument, arg3) == 24, "offset of arg3 must be 24");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(sys_spu_thread_argument_t) == 32, "sizeof sys_spu_thread_argument_t must be 32");
_Static_assert(offsetof(sys_spu_thread_argument_t, arg1) == 0, "offset of arg1 must be 0");
_Static_assert(offsetof(sys_spu_thread_argument_t, arg2) == 8, "offset of arg2 must be 8");
_Static_assert(offsetof(sys_spu_thread_argument_t, arg3) == 16, "offset of arg3 must be 16");
_Static_assert(offsetof(sys_spu_thread_argument_t, arg4) == 24, "offset of arg4 must be 24");

_Static_assert(sizeof(sysSpuThreadArgument) == 32, "sizeof sysSpuThreadArgument must be 32");
_Static_assert(offsetof(sysSpuThreadArgument, arg0) == 0, "offset of arg0 must be 0");
_Static_assert(offsetof(sysSpuThreadArgument, arg1) == 8, "offset of arg1 must be 8");
_Static_assert(offsetof(sysSpuThreadArgument, arg2) == 16, "offset of arg2 must be 16");
_Static_assert(offsetof(sysSpuThreadArgument, arg3) == 24, "offset of arg3 must be 24");
#else
typedef char assert_sizeof_canon[(sizeof(sys_spu_thread_argument_t) == 32) ? 1 : -1];
typedef char assert_off_arg1[(offsetof(sys_spu_thread_argument_t, arg1) == 0) ? 1 : -1];
typedef char assert_off_arg2[(offsetof(sys_spu_thread_argument_t, arg2) == 8) ? 1 : -1];
typedef char assert_off_arg3[(offsetof(sys_spu_thread_argument_t, arg3) == 16) ? 1 : -1];
typedef char assert_off_arg4[(offsetof(sys_spu_thread_argument_t, arg4) == 24) ? 1 : -1];

typedef char assert_sizeof_legacy[(sizeof(sysSpuThreadArgument) == 32) ? 1 : -1];
typedef char assert_off_arg0[(offsetof(sysSpuThreadArgument, arg0) == 0) ? 1 : -1];
typedef char assert_off_leg_arg1[(offsetof(sysSpuThreadArgument, arg1) == 8) ? 1 : -1];
typedef char assert_off_leg_arg2[(offsetof(sysSpuThreadArgument, arg2) == 16) ? 1 : -1];
typedef char assert_off_leg_arg3[(offsetof(sysSpuThreadArgument, arg3) == 24) ? 1 : -1];
#endif

#if !defined(__powerpc__) && !defined(__ppc__) && !defined(__PPU__)
spu_thread_init_record_t g_spu_init_record;
int g_spu_init_ret_val = 0;
#endif

int main(void)
{
    /* 1. Initializer zeroing verification before reassignment */
    sys_spu_thread_argument_t canon_arg;
    canon_arg.arg1 = 0xAAAAAAAAAAAAAAAAULL;
    canon_arg.arg2 = 0xBBBBBBBBBBBBBBBBULL;
    canon_arg.arg3 = 0xCCCCCCCCCCCCCCCCULL;
    canon_arg.arg4 = 0xDDDDDDDDDDDDDDDDULL;

    sys_spu_thread_argument_initialize(canon_arg);
    if (canon_arg.arg1 != 0 || canon_arg.arg2 != 0 ||
        canon_arg.arg3 != 0 || canon_arg.arg4 != 0)
        return 10;

    sysSpuThreadArgument leg_arg;
    leg_arg.arg0 = 0xAAAAAAAAAAAAAAAAULL;
    leg_arg.arg1 = 0xBBBBBBBBBBBBBBBBULL;
    leg_arg.arg2 = 0xCCCCCCCCCCCCCCCCULL;
    leg_arg.arg3 = 0xDDDDDDDDDDDDDDDDULL;

    sysSpuThreadArgumentInitialize(leg_arg);
    if (leg_arg.arg0 != 0 || leg_arg.arg1 != 0 ||
        leg_arg.arg2 != 0 || leg_arg.arg3 != 0)
        return 11;

    /* 2. Assign 4 distinct 64-bit values to canonical argument */
    canon_arg.arg1 = 0x1111222233334444ULL;
    canon_arg.arg2 = 0x5555666677778888ULL;
    canon_arg.arg3 = 0x9999AAAABBBBCCCCULL;
    canon_arg.arg4 = 0xDDDDEEEEFFFF0000ULL;

    if (canon_arg.arg1 != 0x1111222233334444ULL ||
        canon_arg.arg2 != 0x5555666677778888ULL ||
        canon_arg.arg3 != 0x9999AAAABBBBCCCCULL ||
        canon_arg.arg4 != 0xDDDDEEEEFFFF0000ULL)
        return 12;

    /* 3. Call sys_spu_thread_initialize and verify forwarding & return code propagation */
    sys_spu_thread_t thread = 100;
    sys_spu_thread_group_t group = 200;
    sys_spu_image_t img;
    sys_spu_thread_attribute_t attr;

#if !defined(__powerpc__) && !defined(__ppc__) && !defined(__PPU__)
    g_spu_init_ret_val = 0;
    int rc = sys_spu_thread_initialize(&thread, group, 3, &img, &attr, &canon_arg);
    if (rc != 0) return 20;
    if (g_spu_init_record.call_count != 1) return 21;
    if (g_spu_init_record.thread != &thread) return 22;
    if (g_spu_init_record.group != group) return 23;
    if (g_spu_init_record.spu != 3) return 24;
    if (g_spu_init_record.image != (sysSpuImage *)&img) return 25;
    if (g_spu_init_record.attributes != (sysSpuThreadAttribute *)&attr) return 26;
    if (g_spu_init_record.arguments.arg0 != 0x1111222233334444ULL) return 27;
    if (g_spu_init_record.arguments.arg1 != 0x5555666677778888ULL) return 28;
    if (g_spu_init_record.arguments.arg2 != 0x9999AAAABBBBCCCCULL) return 29;
    if (g_spu_init_record.arguments.arg3 != 0xDDDDEEEEFFFF0000ULL) return 30;

    /* Test return code propagation */
    g_spu_init_ret_val = 42;
    rc = sys_spu_thread_initialize(&thread, group, 4, &img, &attr, &canon_arg);
    if (rc != 42) return 31;
    if (g_spu_init_record.call_count != 2) return 32;
    if (g_spu_init_record.spu != 4) return 33;
#else
    /* On PPU cross-compilation, verify inline function call compiles cleanly */
    int rc = sys_spu_thread_initialize(&thread, group, 3, &img, &attr, &canon_arg);
    (void)rc;
#endif

    /* 4. Legacy PSL1GHT pattern */
    leg_arg.arg0 = 0xAAAAULL;
    leg_arg.arg1 = 0xBBBBULL;
    leg_arg.arg2 = 0xCCCCULL;
    leg_arg.arg3 = 0xDDDDULL;

    if (leg_arg.arg0 != 0xAAAAULL || leg_arg.arg1 != 0xBBBBULL ||
        leg_arg.arg2 != 0xCCCCULL || leg_arg.arg3 != 0xDDDDULL)
        return 40;

    /* 5. Positional aggregate initialization */
    sys_spu_thread_argument_t agg = { 1, 2, 3, 4 };
    if (agg.arg1 != 1 || agg.arg2 != 2 || agg.arg3 != 3 || agg.arg4 != 4)
        return 50;

    return 0;
}
