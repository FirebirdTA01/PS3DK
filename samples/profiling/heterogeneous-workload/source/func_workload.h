#ifndef SAMPLE_FUNC_WORKLOAD_H
#define SAMPLE_FUNC_WORKLOAD_H
#include <stdint.h>
/* Stable, separate call paths: root/left/leaf and root/right/leaf. */
uint32_t sample_func_leaf(uint32_t x);
uint32_t sample_func_left(uint32_t x);
uint32_t sample_func_right(uint32_t x);
uint32_t sample_func_root(uint32_t x);
#define SAMPLE_FUNC_ITERATIONS 64u
#define SAMPLE_FUNC_ENTRIES 8u
#define SAMPLE_FUNC_FRAME_SUM UINT32_C(33504)
#endif
