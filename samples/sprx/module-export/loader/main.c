#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>
#include <sys/prx.h>

#include "module_export_contract.h"

SYS_PROCESS_PARAM(1001, 0x10000);

#ifndef MODULE_PRX_PATH
#define MODULE_PRX_PATH "/app_home/sample_module_export.fake.sprx"
#endif

#define EXPECTED_CALC_VALUE (10 + 20 + 0x42)

static void fail_step(const char *step, int32_t rc)
{
	printf("PRX_FAIL step=%s rc=0x%08lx\n", step, (unsigned long)rc);
	exit(1);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("sample-module-loader: loading %s\n", MODULE_PRX_PATH);
	sys_prx_id_t prx_id = sys_prx_load_module(MODULE_PRX_PATH, 0, NULL);
	if (prx_id < 0) {
		fail_step("load", prx_id);
	}
	printf("PRX_LOAD_OK id=0x%08lx\n", (unsigned long)prx_id);

	sample_module_handoff_t handoff;
	memset(&handoff, 0, sizeof(handoff));

	int modres = -1;
	int rc = sys_prx_start_module(prx_id, sizeof(handoff), &handoff, &modres, 0, NULL);
	if (rc != 0 || modres != SYS_PRX_RESIDENT) {
		printf("PRX_FAIL start rc=0x%08lx modres=0x%08lx\n",
		       (unsigned long)rc, (unsigned long)modres);
		return 1;
	}

	if (handoff.magic != SAMPLE_MODULE_MAGIC || handoff.start_counter != 1 || handoff.add == NULL) {
		printf("PRX_FAIL handoff magic=0x%08lx counter=%lu add=%p\n",
		       (unsigned long)handoff.magic,
		       (unsigned long)handoff.start_counter,
		       (void *)(uintptr_t)handoff.add);
		return 1;
	}
	printf("PRX_START_OK counter=%lu\n", (unsigned long)handoff.start_counter);

	int handoff_result = handoff.add(10, 20);
	if (handoff_result != EXPECTED_CALC_VALUE) {
		printf("PRX_FAIL handoff_call got=%d expected=%d\n", handoff_result, EXPECTED_CALC_VALUE);
		return 1;
	}

	int stub_result = sample_calc_add(10, 20);
	if (stub_result != EXPECTED_CALC_VALUE) {
		printf("PRX_FAIL stub_call got=%d expected=%d\n", stub_result, EXPECTED_CALC_VALUE);
		return 1;
	}

	uint32_t counter = sample_get_start_counter();
	if (counter != 1) {
		printf("PRX_FAIL start_counter got=%lu expected=1\n", (unsigned long)counter);
		return 1;
	}
	printf("PRX_EXPORT_CALL_OK val=%d\n", stub_result);

	sample_module_stop_handoff_t stop_handoff;
	memset(&stop_handoff, 0, sizeof(stop_handoff));

	modres = -1;
	rc = sys_prx_stop_module(prx_id, sizeof(stop_handoff), &stop_handoff, &modres, 0, NULL);
	if (rc != 0 || modres != SYS_PRX_STOP_OK ||
	    stop_handoff.magic != SAMPLE_MODULE_STOP_MAGIC ||
	    stop_handoff.stop_counter != 1) {
		printf("PRX_FAIL stop rc=0x%08lx modres=0x%08lx magic=0x%08lx counter=%lu\n",
		       (unsigned long)rc, (unsigned long)modres,
		       (unsigned long)stop_handoff.magic,
		       (unsigned long)stop_handoff.stop_counter);
		return 1;
	}
	printf("PRX_MODULE_STOP_OK\n");

	rc = sys_prx_unload_module(prx_id, 0, NULL);
	if (rc < 0) {
		fail_step("unload", rc);
	}
	printf("PRX_UNLOAD_OK\n");

	return 0;
}
