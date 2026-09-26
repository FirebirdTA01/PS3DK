/* sheap-p1-spu: an SPU initialises and uses a heap on its own (no
 * firmware involved).  Prints SHEAP_P1_OK n/n or SHEAP_P1_FAIL. */
#include "harness.h"
#include "p1_bin.h"

#define P1_STEPS 18

static uint8_t heap[20480] __attribute__((aligned(128)));
static harness_result result;

int main(void)
{
    sysSpuImage image;
    uint64_t args[1][4];
    int32_t status[1];

    memset(heap, 0xee, sizeof(heap));
    args[0][0] = harness_ea(heap);
    args[0][1] = harness_ea(&result);
    args[0][2] = args[0][3] = 0;
    if (sysSpuImageImport(&image, p1_bin, 0) != 0 || harness_run(&image, 1, args, status) != 0) {
        printf("SHEAP_P1_FAIL spu thread setup\n");
        return 0;
    }
    sysSpuImageClose(&image);
    if (status[0] != 0)
        printf("SHEAP_P1_FAIL step %d (%u/%u passed)\n", (int)status[0],
               (unsigned)result.values[0], P1_STEPS);
    else if (result.values[0] != P1_STEPS)
        printf("SHEAP_P1_FAIL %u/%u steps reported\n", (unsigned)result.values[0], P1_STEPS);
    else
        printf("SHEAP_P1_OK %u/%u\n", P1_STEPS, P1_STEPS);
    return 0;
}
