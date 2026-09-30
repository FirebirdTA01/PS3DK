/* What the PPU and SPU parts of hello-daisy share. */
#ifndef HELLO_DAISY_COMMON_H
#define HELLO_DAISY_COMMON_H
#include <stdint.h>

struct Word { uint32_t v, pad[3]; };

/* a consumer's report */
struct Result {
	uint32_t count, faults;
	uint64_t sum;
	uint32_t pad[28];
} __attribute__((aligned(128)));

static const uint32_t DEPTH = 8;
static const uint32_t COUNT = 5000;
#endif
