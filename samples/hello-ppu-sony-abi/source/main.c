/*
 * hello-ppu-sony-abi — Sony reference SDK ABI cross-check.
 *
 * Verifies three concrete pieces of ABI alignment between our toolchain
 * output and what Sony's PS3 loader expects:
 *
 *   1. A .sys_proc_param section is emitted, 8-byte aligned, containing
 *      a 36-byte struct with magic=0x13bcc5f6, version=0x00330000.
 *      PSL1GHT's SYS_PROCESS_PARAM produces 32 bytes with version
 *      0x00009000 — visibly different, so we cannot rely on it here.
 *
 *   2. newlib's malloc / free / calloc / realloc work through the
 *      libsysbase sbrk_r dispatch layer.  Anything that compiles here
 *      would succeed regardless, but the sample is meant to be *run*
 *      under RPCS3 to confirm the allocator is actually functional.
 *
 *   3. printf routes through libsysbase _write_r -> __syscalls.write_r
 *      (installed by PSL1GHT crt1).  A bogus route would show garbled
 *      output on RPCS3 stdout; a missing route would hang.
 *
 * This sample is intentionally a C file (not C++) so any libstdc++
 * entanglement is isolated in hello-ppu-c++17 and cannot mask a pure-
 * C ABI regression here.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sony_process_param.h"

SONY_PROCESS_PARAM(1001, 0x10000);

static const uint32_t kExpectedMagic = SONY_PROCESS_PARAM_MAGIC;
static const uint32_t kExpectedVersion = SONY_PROCESS_PARAM_VERSION_330_0;
static const size_t   kExpectedStructSize = 36;

extern sony_process_param_t __sys_process_param;

static int check_proc_param(void)
{
	int ok = 1;

	printf("  __sys_process_param @ %p\n", (void *)&__sys_process_param);
	printf("  size=%u (expected %zu)\n",
	       (unsigned)__sys_process_param.size, kExpectedStructSize);
	printf("  magic=0x%08x (expected 0x%08x)\n",
	       (unsigned)__sys_process_param.magic, (unsigned)kExpectedMagic);
	printf("  version=0x%08x (expected 0x%08x)\n",
	       (unsigned)__sys_process_param.version, (unsigned)kExpectedVersion);
	printf("  primary_prio=%d stacksize=0x%x\n",
	       (int)__sys_process_param.primary_prio,
	       (unsigned)__sys_process_param.primary_stacksize);

	if (sizeof(sony_process_param_t) != kExpectedStructSize) {
		printf("  FAIL: sizeof wrong (%zu vs %zu)\n",
		       sizeof(sony_process_param_t), kExpectedStructSize);
		ok = 0;
	}
	if (__sys_process_param.size != kExpectedStructSize) ok = 0;
	if (__sys_process_param.magic != kExpectedMagic) ok = 0;
	if (__sys_process_param.version != kExpectedVersion) ok = 0;

	return ok;
}

static int check_allocator(void)
{
	enum { kBlockSize = 32 * 1024, kBlockCount = 8 };
	void *blocks[kBlockCount];
	int i;
	int ok = 1;

	for (i = 0; i < kBlockCount; i++) {
		blocks[i] = malloc(kBlockSize);
		if (!blocks[i]) {
			printf("  malloc #%d failed\n", i);
			ok = 0;
			break;
		}
		memset(blocks[i], (char)(0xA0 + i), kBlockSize);
	}
	for (int j = 0; j < i; j++) free(blocks[j]);

	void *c = calloc(1, kBlockSize);
	if (!c) { printf("  calloc failed\n"); return 0; }
	for (size_t k = 0; k < 64; k++) {
		if (((unsigned char *)c)[k] != 0) { ok = 0; break; }
	}

	void *r = realloc(c, 2 * kBlockSize);
	if (!r) { printf("  realloc failed\n"); free(c); return 0; }
	free(r);

	if (ok) printf("  allocator OK (%d x %d-byte blocks, calloc zeroed, realloc grew)\n",
	               kBlockCount, kBlockSize);
	return ok;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	printf("hello-ppu-sony-abi\n");

	printf("proc_param:\n");
	int param_ok = check_proc_param();

	printf("allocator:\n");
	int alloc_ok = check_allocator();

	printf("result: %s\n", (param_ok && alloc_ok) ? "PASS" : "FAIL");
	return (param_ok && alloc_ok) ? 0 : 1;
}
