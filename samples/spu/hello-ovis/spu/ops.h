/* The overlaid functions.  Each lives in its own source file, so each can be
 * its own overlay section: add and mul share one LS range, fib and gcd
 * another. */
#ifndef HELLO_OVIS_OPS_H
#define HELLO_OVIS_OPS_H
#include <stdint.h>

uint32_t op_add(uint32_t a, uint32_t b);
uint32_t op_mul(uint32_t a, uint32_t b);
uint32_t op_fib(uint32_t n);
uint32_t op_gcd(uint32_t a, uint32_t b);

/* what the SPU program sends back to the PPU */
typedef struct {
	uint32_t results[8];   /* add, mul, fib, gcd, then the same again */
	uint32_t abort_rc;     /* manual: StartMapping of a resident section */
	uint32_t mapped[4];    /* manual: resident flags after the last calls */
	uint32_t novlys;
	uint32_t pad[18];
} __attribute__((aligned(128))) ovis_report_t;
#endif
