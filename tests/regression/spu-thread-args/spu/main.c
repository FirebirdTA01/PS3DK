/* spu-thread-args (SPU side): main receives the four 64-bit words of the
 * thread's sys_spu_thread_argument from the startup code.
 *
 * Exits with 0 when all four match, otherwise with a bitmask of the wrong
 * arguments (bit 0 = first ... bit 3 = fourth); the PPU side prints it.
 */
#include <stdint.h>
#include <sys/spu_thread.h>

#define ARG1 0x0123456789abcdefull
#define ARG2 0xfedcba9876543210ull
#define ARG3 0x1111222233334444ull
#define ARG4 0x5555666677778888ull

int main(uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    int mask = 0;

    if (a1 != ARG1)
        mask |= 1;
    if (a2 != ARG2)
        mask |= 2;
    if (a3 != ARG3)
        mask |= 4;
    if (a4 != ARG4)
        mask |= 8;

    sys_spu_thread_exit(mask);
    return 0;
}
