/* The source-only -100 scaffold established parent gate103 RED. This fixture
 * now uses the actual schema2 transaction, dirty prepared arenas and relocation
 * control. C receives ordinary bytes only, never expected checkpoint bytes. */
#define COMBINED2_ENCODER_TEST_CONFIG_H
#include "roll_combined2.h"
static pfm2_config test_config(void)
{
    pfm2_config c={{16,16,32,64,8,8,8,32,64,64,32,32,32,64,131072,262144,UINT64_C(1)<<28},
        8,8,32,8,192,64,64,64,64,4096,131072};
    return c;
}
#include "combined2_encoder_fixture.c"
