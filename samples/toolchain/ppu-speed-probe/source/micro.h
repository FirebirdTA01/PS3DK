#pragma once
#include <stdint.h>

// Microkernels (micro.cpp).  Each takes the probe's random generator and
// returns a checksum of its results.
uint32_t k_micro_sext(uint32_t (*rng)());
uint32_t k_micro_flags(uint32_t (*rng)());
uint32_t k_micro_preds(uint32_t (*rng)());
uint32_t k_micro_shortloops(uint32_t (*rng)());
uint32_t k_micro_lanes(uint32_t (*rng)());
uint32_t k_micro_imports(uint32_t (*rng)());
