#include <stddef.h>

#include <sys/prx_module.h>

#include "hello_prx_contract.h"

int prx_add(int lhs, int rhs)
{
	return lhs + rhs + 0x1200;
}

int module_start(size_t args, void *argp)
{
	if (args >= sizeof(hello_prx_handoff_t) && argp != NULL) {
		hello_prx_handoff_t *handoff = (hello_prx_handoff_t *)argp;
		handoff->sentinel = HELLO_PRX_SENTINEL;
		handoff->version = 1;
		handoff->add = prx_add;
		handoff->started = 1;
	}

	return SYS_PRX_RESIDENT;
}

int module_stop(size_t args, void *argp)
{
	(void)args;
	(void)argp;
	return SYS_PRX_RESIDENT;
}
