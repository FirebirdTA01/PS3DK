/* Judge, built at -O0 against a literal table. */
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

extern void vect_realign_fill(uint32_t out[64]);

static const uint32_t want[4] = {0x13ff0000, 0x1300ff00, 0x130000ff, 0x13ffff00};

int main(void)
{
    static volatile uint32_t got[64];
    uint32_t tmp[64];
    vect_realign_fill(tmp);
    for (unsigned i = 0; i < 64; ++i) got[i] = tmp[i];
    printf("VECT_REALIGN rows %08x %08x %08x %08x\n",
           (unsigned)got[0], (unsigned)got[16], (unsigned)got[32], (unsigned)got[48]);
    unsigned bad = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (got[i] != want[i / 16]) {
            if (bad < 8) printf("VECT_REALIGN mismatch word=%u got=%08x expected=%08x\n",
                                i, (unsigned)got[i], (unsigned)want[i / 16]);
            ++bad;
        }
    printf("VECT_REALIGN checks=64 failures=%u\n", bad);
    printf(bad ? "VECT_REALIGN_FAIL\n" : "VECT_REALIGN_OK\n");
    return bad ? 1 : 0;
}
