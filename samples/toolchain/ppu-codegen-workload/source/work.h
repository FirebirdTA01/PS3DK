#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { WORK_PHASES = 6 };
extern const char *const work_phase_names[WORK_PHASES];

// Build the inputs once; 0 on success.
int work_init(void);
// One frame's PPU work for a phase; returns a checksum of the results.
uint32_t work_frame(int phase);

#ifdef __cplusplus
}
#endif
