/* lock_line.c - the reservation buffer shared by every object operation. */
#include "sync2_internal.h"

volatile uint8_t __sync2_line[128] __attribute__((aligned(128))) = { 0 };
