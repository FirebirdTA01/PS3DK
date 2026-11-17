#ifndef COMBINED_ENCODER_TEST_CONFIG_H
#define COMBINED_ENCODER_TEST_CONFIG_H
#include "roll_combined.h"
static pfm_config test_config(void)
{
    pfm_config c={16,16,32,64,8,8,8,32,64,64,32,32,32,64,32784,65536,1048576};
    return c;
}
#endif
