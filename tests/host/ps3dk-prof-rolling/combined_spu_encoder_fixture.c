/* Positive SPU parity uses roomy, explicitly bounded capacities. Capacity
 * refusal tests are a later independent RED unit. Reuse the unchanged whole
 * original-source driver and its relocation/immutability checks. */
#include "roll_combined.h"
static pfm_config test_config(void)
{
    pfm_config c={16,16,32,64,8,8,8,32,128,64,128,128,32,256,65552,131072,4194304};
    return c;
}
#define COMBINED_ENCODER_TEST_CONFIG_H
#include "combined_encoder_fixture.c"
