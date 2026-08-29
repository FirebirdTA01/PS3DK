#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/process.h>

#include "hello_prx_contract.h"

SYS_PROCESS_PARAM(1001, 0x10000);

/* A module's container has to match the executable's.  ps3_add_prx(... SIGN)
 * emits both, mirroring ps3_add_self:
 *
 *   <target>.self       pairs with   hello-sprx-export.sprx        (real-signed)
 *   <target>.fake.self  pairs with   hello-sprx-export.fake.sprx   (fself)
 *
 * This sample is booted as .fake.self, so it loads the .fake.sprx.  Ship the
 * real .self and load the real .sprx instead -- override at configure time:
 *   -DHELLO_PRX_PATH='"/app_home/hello-sprx-export.sprx"'
 * or point it at the unsigned .prx, which loads only when the main executable
 * is booted as a raw .elf (RPCS3 decrypt_self behaviour).
 */
#ifndef HELLO_PRX_PATH
#define HELLO_PRX_PATH "/app_home/hello-sprx-export.fake.sprx"
#endif
#define EXPECTED_ADD_VALUE (20 + 22 + 0x1200)

typedef int32_t sysPrxId;
typedef uint64_t sysPrxFlags;
typedef struct sysPrxLoadModuleOption sysPrxLoadModuleOption;
typedef struct sysPrxStartModuleOption sysPrxStartModuleOption;
typedef struct sysPrxStopModuleOption sysPrxStopModuleOption;

sysPrxId sysPrxLoadModule(const char *path, sysPrxFlags flags, sysPrxLoadModuleOption *opt);
int32_t sysPrxStartModule(sysPrxId id, size_t args, void *argp, int32_t *modres,
                          sysPrxFlags flags, sysPrxStartModuleOption *opt);
int32_t sysPrxStopModule(sysPrxId id, size_t args, void *argp, int32_t *modres,
                         sysPrxFlags flags, sysPrxStopModuleOption *opt);
sysPrxId sysPrxUnloadModule(sysPrxId id, sysPrxFlags flags, sysPrxLoadModuleOption *opt);

static void fail_prx(const char *step, int32_t rc)
{
	printf("PRX_FAIL %s rc=0x%08lx\n", step, (unsigned long)rc);
	exit(1);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	hello_prx_handoff_t handoff;
	memset(&handoff, 0, sizeof(handoff));

	printf("hello-sprx-import: loading %s\n", HELLO_PRX_PATH);
	sysPrxId prx_id = sysPrxLoadModule(HELLO_PRX_PATH, 0, NULL);
	if (prx_id < 0) {
		fail_prx("load", prx_id);
	}

	int32_t modres = -1;
	int32_t rc = sysPrxStartModule(prx_id, sizeof(handoff), &handoff, &modres, 0, NULL);
	if (rc != 0 || modres != 0) {
		printf("PRX_FAIL start rc=0x%08lx modres=0x%08lx\n",
		       (unsigned long)rc, (unsigned long)modres);
		return 1;
	}

	if (handoff.sentinel != HELLO_PRX_SENTINEL || handoff.started != 1 || handoff.add == NULL) {
		printf("PRX_FAIL handoff sentinel=0x%08lx started=%d add=%p\n",
		       (unsigned long)handoff.sentinel, handoff.started, (void *)(uintptr_t)handoff.add);
		return 1;
	}

	int handoff_value = handoff.add(20, 22);
	if (handoff_value != EXPECTED_ADD_VALUE) {
		printf("PRX_FAIL handoff_call handoff=%d expected=%d\n",
		       handoff_value, EXPECTED_ADD_VALUE);
		return 1;
	}

	printf("PRX_OK sentinel=0x%08lx handoff=%d\n",
	       (unsigned long)handoff.sentinel, handoff_value);

	int import_value = prx_add(20, 22);
	if (import_value != EXPECTED_ADD_VALUE) {
		printf("PRX_IMPORT_FAIL import=%d expected=%d\n", import_value, EXPECTED_ADD_VALUE);
		return 1;
	}

	printf("PRX_IMPORT_OK import=%d\n", import_value);

	modres = -1;
	rc = sysPrxStopModule(prx_id, 0, NULL, &modres, 0, NULL);
	if (rc != 0) {
		printf("PRX_WARN stop rc=0x%08lx modres=0x%08lx\n",
		       (unsigned long)rc, (unsigned long)modres);
	}

	rc = sysPrxUnloadModule(prx_id, 0, NULL);
	if (rc != 0) {
		printf("PRX_WARN unload rc=0x%08lx\n", (unsigned long)rc);
	}

	return 0;
}
