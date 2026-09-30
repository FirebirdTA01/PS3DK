#include "ops.h"
uint32_t op_gcd(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;
		a = b;
		b = t;
	}
	return a;
}
