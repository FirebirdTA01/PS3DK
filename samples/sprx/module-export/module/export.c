#include <stddef.h>
#include <stdint.h>
#include <sys/prx.h>
#include <sys/moduleexport.h>

#include "module_export_contract.h"

SYS_MODULE_INFO(sample_module_export, 0, 1, 0);
SYS_MODULE_START(sample_module_start);
SYS_MODULE_STOP(sample_module_stop);

static uint32_t g_start_calls = 0;
static uint32_t g_stop_calls = 0;

int sample_calc_add(int a, int b)
{
	return a + b + 0x42;
}

uint32_t sample_get_start_counter(void)
{
	return g_start_calls;
}

int sample_module_start(size_t args, void *argp)
{
	g_start_calls++;
	if (args >= sizeof(sample_module_handoff_t) && argp != NULL) {
		sample_module_handoff_t *handoff = (sample_module_handoff_t *)argp;
		handoff->magic = SAMPLE_MODULE_MAGIC;
		handoff->start_counter = g_start_calls;
		handoff->add = sample_calc_add;
	}
	return SYS_PRX_RESIDENT;
}

int sample_module_stop(size_t args, void *argp)
{
	g_stop_calls++;
	if (args >= sizeof(sample_module_stop_handoff_t) && argp != NULL) {
		sample_module_stop_handoff_t *stop_handoff = (sample_module_stop_handoff_t *)argp;
		stop_handoff->magic = SAMPLE_MODULE_STOP_MAGIC;
		stop_handoff->stop_counter = g_stop_calls;
	}
	return SYS_PRX_STOP_OK;
}
