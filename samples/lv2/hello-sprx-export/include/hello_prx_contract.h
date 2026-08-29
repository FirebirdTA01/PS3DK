#ifndef HELLO_PRX_CONTRACT_H
#define HELLO_PRX_CONTRACT_H

#include <stdint.h>

#define HELLO_PRX_SENTINEL 0x5052584fU

typedef int (*hello_prx_add_fn)(int lhs, int rhs);

typedef struct hello_prx_handoff {
	uint32_t sentinel;
	uint32_t version;
	hello_prx_add_fn add;
	int started;
} hello_prx_handoff_t;

int prx_add(int lhs, int rhs);

#endif /* HELLO_PRX_CONTRACT_H */
