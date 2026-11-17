#ifndef COMBINED2_ENCODER_TEST_CONFIG_H
#define COMBINED2_ENCODER_TEST_CONFIG_H
#include "roll_combined2.h"
static pfm2_config test_config(void)
{
    pfm2_config c={{16,16,32,64,8,8,8,32,64,64,32,32,32,64,32784,65536,1048576},8,8,32,8};
    return c;
}
#endif
