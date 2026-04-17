/*
 * hello-ppu-sony-abi — Sony reference SDK ABI cross-check.
 *
 * Verifies three concrete pieces of ABI alignment between our toolchain
 * output and what Sony's PS3 loader expects:
 *
 *   1. A .sys_proc_param section is emitted, 8-byte aligned, containing
 *      a 36-byte struct with magic=0x13bcc5f6, version=0x00330000, and
 *      a trailing crash_dump_param_addr word.  This now uses PSL1GHT's
 *      native SYS_PROCESS_PARAM macro from <sys/process.h> — the
 *      previously-separate <sys/sony_process_param.h> shim has been
 *      retired in favour of making the PSL1GHT header Sony-correct.
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
 *   4. crash_dump_param_addr is filled in at link time with the low 32
 *      bits of our user-defined __sys_process_crash_dump_param callback.
 *      The PSL1GHT SYS_PROCESS_PARAM macro emits the struct via inline
 *      asm with a R_PPC64_ADDR32 relocation against the (weak) callback
 *      symbol — defining the symbol here in the sample makes the
 *      relocation resolve to a real address; omitting it leaves the
 *      field at 0 (Sony's "no callback" value).
 *
 * This sample is intentionally a C file (not C++) so any libstdc++
 * entanglement is isolated in hello-ppu-c++17 and cannot mask a pure-
 * C ABI regression here.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* User-defined crash-dump callback.  When this symbol is present the PSL1GHT
 * SYS_PROCESS_PARAM macro's R_PPC64_ADDR32 relocation resolves to its OPD
 * descriptor address (the low 32 bits of which go into
 * __sys_process_param.crash_dump_param_addr).  In a real app the loader
 * would call this on process crash; for the ABI smoke test we just verify
 * the pointer was wired up correctly. */
void __sys_process_crash_dump_param(void) { }

static const uint32_t kExpectedMagic = SYS_PROCESS_SPAWN_MAGIC;
static const uint32_t kExpectedVersion = SYS_PROCESS_SPAWN_VERSION_330;
static const size_t   kExpectedStructSize = 36;

extern sys_process_param_t __sys_process_param;

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
	printf("  prio=%d stacksize=0x%x crash_dump=0x%x\n",
	       (int)__sys_process_param.prio,
	       (unsigned)__sys_process_param.stacksize,
	       (unsigned)__sys_process_param.crash_dump_param_addr);

	if (sizeof(sys_process_param_t) != kExpectedStructSize) {
		printf("  FAIL: sizeof wrong (%zu vs %zu)\n",
		       sizeof(sys_process_param_t), kExpectedStructSize);
		ok = 0;
	}
	if (__sys_process_param.size != kExpectedStructSize) ok = 0;
	if (__sys_process_param.magic != kExpectedMagic) ok = 0;
	if (__sys_process_param.version != kExpectedVersion) ok = 0;

	/* Confirm the crash-dump relocation actually fired.  The 32-bit field
	 * should hold the low 32 bits of __sys_process_crash_dump_param's OPD
	 * descriptor address.  If the relocation silently filled in 0 instead,
	 * task #7's mechanism has regressed. */
	uintptr_t cb_addr = (uintptr_t)&__sys_process_crash_dump_param;
	uint32_t  expected_crash = (uint32_t)cb_addr;
	if (__sys_process_param.crash_dump_param_addr != expected_crash) {
		printf("  FAIL: crash_dump_param_addr=0x%08x, expected 0x%08x\n",
		       (unsigned)__sys_process_param.crash_dump_param_addr,
		       (unsigned)expected_crash);
		ok = 0;
	} else if (expected_crash == 0) {
		printf("  WARN: crash-dump callback resolved to 0 — unexpected\n");
		ok = 0;
	}

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
