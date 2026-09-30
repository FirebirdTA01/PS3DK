#include "ops.h"
uint32_t op_fib(uint32_t n)
{
	uint32_t a = 0, b = 1;
	while (n--) {
		uint32_t t = a + b;
		a = b;
		b = t;
	}
	return a;
}
