#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include <sys/process.h>
#include <cell/face.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    uint64_t dict_ea = cellFaceDetectionGetDictEa();
    printf("cellFaceDetectionGetDictEa -> 0x%016" PRIx64 "\n", dict_ea);

    CellFaceAgeRange range = {0};
    CellFaceAgeDistr distr = {0};
    uint32_t age = 0;
    uint32_t min_age = 0;
    uint32_t max_age = 0;

    int32_t rc = cellFaceAgeRangeIntegrate(
        &range, &distr, &age, &min_age, &max_age);
    printf("cellFaceAgeRangeIntegrate -> 0x%08x\n", (unsigned)rc);

    return rc == CELL_OK ? 0 : 1;
}
