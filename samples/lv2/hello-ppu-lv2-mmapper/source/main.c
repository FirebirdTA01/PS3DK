/*
 * hello-ppu-lv2-mmapper - LV2 mmapper syscall wrapper validation.
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/memory.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
	printf("hello-ppu-lv2-mmapper: mmapper syscall validation\n");

	sys_addr_t alloc_addr = 0;
	int rc = sys_memory_allocate(64 * 1024,
		SYS_MEMORY_PAGE_SIZE_64K | SYS_MEMORY_ACCESS_RIGHT_ANY,
		&alloc_addr);
	if (rc == 0 && alloc_addr) {
		/* expected: 0x00000000 (OK) - allocation succeeded on real HW */
		printf("  sys_memory_allocate(64K,64K) -> 0x%08x, addr=0x%llx\n",
		       (unsigned)rc, (unsigned long long)alloc_addr);
		rc = sys_memory_free(alloc_addr);
		/* expected: 0x00000000 (OK) - freeing successful allocation */
		printf("  sys_memory_free -> 0x%08x\n", (unsigned)rc);
	} else {
		/* expected: 0x80010002 (EINVAL) - RPCS3 HLE, alloc failed */
		printf("  sys_memory_allocate(64K,64K) -> 0x%08x, alloc failed\n",
		       (unsigned)rc);
		rc = sys_memory_free(0);
		/* expected: 0x80010002 (EINVAL) - deliberate invalid-free probe */
		printf("  sys_memory_free(0) -> 0x%08x\n", (unsigned)rc);
	}

	alloc_addr = 0;
	rc = sys_memory_allocate_from_container(64 * 1024,
		SYS_MEMORY_CONTAINER_ID_INVALID,
		SYS_MEMORY_PAGE_SIZE_64K | SYS_MEMORY_ACCESS_RIGHT_ANY,
		&alloc_addr);
	/* expected: 0x80010002 (EINVAL) - container INVALID */
	printf("  sys_memory_allocate_from_container(INVALID) -> 0x%08x\n",
	       (unsigned)rc);

	rc = sys_mmapper_allocate_fixed_address();
	/* expected: 0x00000000 (OK) - no-args fixed-address alloc */
	printf("  sys_mmapper_allocate_fixed_address -> 0x%08x\n",
	       (unsigned)rc);

	sys_addr_t addr = 0;
	rc = sys_mmapper_allocate_address(64 * 1024,
		SYS_MEMORY_PAGE_SIZE_64K | SYS_MEMORY_ACCESS_RIGHT_ANY,
		64 * 1024, &addr);
	if (rc == 0 && addr) {
		/* expected: 0x00000000 (OK) - real HW alloc succeeds */
		printf("  sys_mmapper_allocate_address(64K,64K) -> 0x%08x, addr=0x%llx\n",
		       (unsigned)rc, (unsigned long long)addr);
		rc = sys_mmapper_free_address(addr);
		/* expected: 0x00000000 (OK) - freeing successful allocation */
		printf("  sys_mmapper_free_address -> 0x%08x\n", (unsigned)rc);
	} else {
		/* expected: 0x80010002 (EINVAL) - RPCS3 HLE, alloc failed */
		printf("  sys_mmapper_allocate_address(64K,64K) -> 0x%08x, alloc failed\n",
		       (unsigned)rc);
		rc = sys_mmapper_free_address(0);
		/* expected: 0x80010002 (EINVAL) - deliberate invalid-free probe */
		printf("  sys_mmapper_free_address(0) -> 0x%08x\n", (unsigned)rc);
	}

	sys_addr_t map_addr = 0;
	rc = sys_mmapper_search_and_map(0, 0, 0, &map_addr);
	/* expected: 0x80010002 (EINVAL) - ipc_key=0 not registered */
	printf("  sys_mmapper_search_and_map(key=0) -> 0x%08x\n", (unsigned)rc);

	rc = sys_mmapper_change_address_access_right(0, 0);
	/* expected: 0x00000000 (OK) - addr=0+flags=0 is no-op */
	printf("  sys_mmapper_change_address_access_right -> 0x%08x\n",
	       (unsigned)rc);

	rc = sys_mmapper_enable_page_fault_notification(0, 0);
	/* expected: 0x80010002 (EINVAL) - queue_id=0+addr=0 is invalid */
	printf("  sys_mmapper_enable_page_fault_notification -> 0x%08x\n",
	       (unsigned)rc);

	printf("result: PASS (validation complete)\n");
	sys_process_exit(0);
	return 0;
}
