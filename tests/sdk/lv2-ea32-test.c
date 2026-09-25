/* Shared C/C++ probe: compile on PPU; execute with a host compiler. */
#include <sys/lv2_types.h>

#ifdef EXPECT_POINTER_SIZE
typedef char pointer_width_matches[sizeof(void *) == EXPECT_POINTER_SIZE ? 1 : -1];
typedef char uintptr_width_matches[sizeof(uintptr_t) == EXPECT_POINTER_SIZE ? 1 : -1];
#endif

int main(int argc, char **argv)
{
    static const uint32_t values[] = {0, 1, 0x7fffffffU, 0x80000000U, 0xffffffffU};
    unsigned int i;
    (void)argv;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        void *p = lv2_ea32_expand(values[i]);
        if ((uintptr_t)p != values[i] || lv2_ea32_pack(p) != values[i])
            return 1;
    }
#if UINTPTR_MAX > UINT32_MAX
    if (argc > 1) {
        /* Debug must trap; release must retain exactly the low 32 bits. */
        volatile uintptr_t wide = UINT64_C(0x100000005);
        return lv2_ea32_pack((const void *)wide) == 5 ? 0 : 2;
    }
#else
    (void)argc;
#endif
    return 0;
}
