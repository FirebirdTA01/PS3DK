/*
 * hello-ppu-lv2-vm - on-demand memory: LV2 virtual memory and the mmapper.
 *
 * Part 1 (sys_vm_*): reserve 32 MB of address space backed by 1 MB of
 * physical memory and write a pattern over 4 MB of it; the kernel pages
 * between them on demand.  Then read the pattern back, query page state
 * and statistics, and unmap.
 *
 * Part 2 (sys_mmapper_*): reserve a 256 MB address range, allocate a 1 MB
 * memory object, map it into the range, use it, unmap it (getting its id
 * back), map it again elsewhere in the range and see the same bytes, then
 * free everything.
 *
 * The last TTY line is "hello-ppu-lv2-vm: PASS (<n> checks)" or names the
 * failures.
 */
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/memory.h>
#include <sys/vm.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define MB (1024u * 1024u)

static int checks, failures;

static void check(int ok, const char *what, unsigned long long got)
{
	checks++;
	if (!ok)
		failures++;
	printf("  %s %s (0x%llx)\n", ok ? "ok  " : "FAIL", what, got);
}

static uint32_t pattern(uint32_t i) { return i * 2654435761u ^ 0x5a5a5a5au; }

static void part_vm(void)
{
	sys_memory_container_t cid;
	sys_addr_t base = 0;
	int rc;

	printf("part 1: sys_vm_* - 32 MB of address space over 1 MB of memory\n");
	rc = sys_memory_container_create(&cid, 1 * MB);
	check(rc == 0, "sys_memory_container_create(1 MB)", (unsigned)rc);

	rc = sys_vm_memory_map(32 * MB, 1 * MB, cid, SYS_MEMORY_PAGE_SIZE_64K,
	                       SYS_VM_POLICY_AUTO_RECOMMENDED, &base);
	check(rc == 0 && base != 0, "sys_vm_memory_map(32 MB virtual, 1 MB physical)", base);
	if (rc != 0) {
		sys_memory_container_destroy(cid);
		return;
	}

	/* 4 MB of writes through 1 MB of physical memory. */
	volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)base;
	const uint32_t words = 4 * MB / 4;
	for (uint32_t i = 0; i < words; i += 1024)
		p[i] = pattern(i);
	uint32_t bad = 0;
	for (uint32_t i = 0; i < words; i += 1024)
		bad += p[i] != pattern(i);
	check(bad == 0, "4 MB pattern reads back through 1 MB of physical memory", bad);

	uint64_t state = 0;
	rc = sys_vm_touch(base, 64 * 1024);
	check(rc == 0, "sys_vm_touch(first 64 KB)", (unsigned)rc);
	rc = sys_vm_test(base, 64 * 1024, &state);
	check(rc == 0 && state == SYS_VM_STATE_ON_MEMORY,
	      "sys_vm_test: a touched page is on memory", state);

	rc = sys_vm_lock(base, 64 * 1024);
	check(rc == 0, "sys_vm_lock", (unsigned)rc);
	rc = sys_vm_unlock(base, 64 * 1024);
	check(rc == 0, "sys_vm_unlock", (unsigned)rc);
	rc = sys_vm_sync(base, 64 * 1024);
	check(rc == 0, "sys_vm_sync", (unsigned)rc);

	sys_vm_statistics_t st;
	rc = sys_vm_get_statistics(base, &st);
	check(rc == 0 && st.pmem_total == 1 * MB, "sys_vm_get_statistics: 1 MB of physical memory",
	      st.pmem_total);
	printf("  stats: ppu faults %llu, page in %llu, page out %llu, pmem used %u\n",
	       (unsigned long long)st.page_fault_ppu, (unsigned long long)st.page_in,
	       (unsigned long long)st.page_out, st.pmem_used);

	rc = sys_vm_unmap(base);
	check(rc == 0, "sys_vm_unmap", (unsigned)rc);
	rc = sys_memory_container_destroy(cid);
	check(rc == 0, "sys_memory_container_destroy", (unsigned)rc);
}

static void part_mmapper(void)
{
	sys_addr_t range = 0;
	sys_memory_t mem = 0, back = 0;
	sys_page_attr_t attr;
	int rc;

	printf("part 2: sys_mmapper_* - map one memory object at two addresses\n");
	rc = sys_mmapper_allocate_address(256 * MB, SYS_MEMORY_PAGE_SIZE_1M, 256 * MB, &range);
	check(rc == 0 && range != 0, "sys_mmapper_allocate_address(256 MB)", range);
	if (rc != 0)
		return;

	rc = sys_mmapper_allocate_memory(1 * MB, SYS_MEMORY_GRANULARITY_1M, &mem);
	check(rc == 0, "sys_mmapper_allocate_memory(1 MB)", (unsigned)rc);

	rc = sys_mmapper_map_memory(range, mem, SYS_MEMORY_PROT_READ_WRITE);
	check(rc == 0, "sys_mmapper_map_memory at the start of the range", (unsigned)rc);
	if (rc == 0) {
		volatile uint32_t *a = (volatile uint32_t *)(uintptr_t)range;
		a[0] = 0xc0ffee01u;
		a[MB / 4 - 1] = 0xc0ffee02u;

		rc = sys_memory_get_page_attribute(range, &attr);
		check(rc == 0 && attr.page_size == 1 * MB,
		      "sys_memory_get_page_attribute: a 1 MB page", attr.page_size);

		rc = sys_mmapper_unmap_memory(range, &back);
		check(rc == 0 && back == mem, "sys_mmapper_unmap_memory returns the object", back);

		sys_addr_t second = range + 16 * MB;
		rc = sys_mmapper_map_memory(second, mem, SYS_MEMORY_PROT_READ_WRITE);
		check(rc == 0, "sys_mmapper_map_memory 16 MB further on", (unsigned)rc);
		if (rc == 0) {
			volatile uint32_t *b = (volatile uint32_t *)(uintptr_t)second;
			check(b[0] == 0xc0ffee01u && b[MB / 4 - 1] == 0xc0ffee02u,
			      "the same bytes at the second address", b[0]);
			rc = sys_mmapper_unmap_memory(second, &back);
			check(rc == 0 && back == mem, "unmap the second mapping", back);
		}
	}

	rc = sys_mmapper_free_memory(mem);
	check(rc == 0, "sys_mmapper_free_memory", (unsigned)rc);
	rc = sys_mmapper_free_address(range);
	check(rc == 0, "sys_mmapper_free_address", (unsigned)rc);
}

int main(void)
{
	printf("hello-ppu-lv2-vm: on-demand memory\n");
	part_vm();
	part_mmapper();
	if (failures)
		printf("hello-ppu-lv2-vm: FAIL (%d of %d checks)\n", failures, checks);
	else
		printf("hello-ppu-lv2-vm: PASS (%d checks)\n", checks);
	return failures ? 1 : 0;
}
