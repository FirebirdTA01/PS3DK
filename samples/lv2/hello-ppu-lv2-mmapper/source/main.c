/* LV2 memory allocation and mmapper wrapper validation. */
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/memory.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* Raw LV2 invalid-argument result; not the host/newlib errno value. */
#define LV2_INVALID_ARGUMENT ((int)0x80010002u)
#define LV2_NO_SUCH_CONTAINER ((int)0x80010005u)

static int failures;

static int expect(const char *name, int rc, int wanted)
{
    printf("  %s -> 0x%08x (expected 0x%08x)%s\n", name,
           (unsigned)rc, (unsigned)wanted, rc == wanted ? "" : " FAIL");
    if (rc != wanted) ++failures;
    return rc == wanted;
}

int main(void)
{
    printf("hello-ppu-lv2-mmapper: mmapper syscall validation\n");
    sys_addr_t alloc_addr = 0;
    /* Allocation flags select page size; access-right bits are not accepted
       by sys_memory_allocate. */
    int rc = sys_memory_allocate(64 * 1024, SYS_MEMORY_PAGE_SIZE_64K,
                                 &alloc_addr);
    if (expect("sys_memory_allocate(64K,64K)", rc, CELL_OK)) {
        if (!alloc_addr) {
            puts("  FAIL allocation returned a null address");
            ++failures;
        } else {
            volatile uint32_t *word = (volatile uint32_t *)(uintptr_t)alloc_addr;
            *word = 0x12345678;
            if (*word != 0x12345678) ++failures;
            expect("sys_memory_free", sys_memory_free(alloc_addr), CELL_OK);
        }
    }
    expect("sys_memory_free(0)", sys_memory_free(0), LV2_INVALID_ARGUMENT);
    alloc_addr = 0;
    expect("sys_memory_allocate_from_container(INVALID)",
           sys_memory_allocate_from_container(64 * 1024,
               SYS_MEMORY_CONTAINER_ID_INVALID, SYS_MEMORY_PAGE_SIZE_64K,
               &alloc_addr), LV2_NO_SUCH_CONTAINER);

    expect("sys_mmapper_allocate_fixed_address",
           sys_mmapper_allocate_fixed_address(), CELL_OK);
    /* This reserves virtual address space, not physical memory. The address
       allocator requires 256 MiB size/alignment, not the backing page size. */
    sys_addr_t addr = 0;
    rc = sys_mmapper_allocate_address(0x10000000, SYS_MEMORY_PAGE_SIZE_64K,
                                     0x10000000, &addr);
    if (expect("sys_mmapper_allocate_address(256MiB)", rc, CELL_OK)) {
        if (!addr) {
            puts("  FAIL reservation returned a null address");
            ++failures;
        } else {
            expect("sys_mmapper_free_address", sys_mmapper_free_address(addr),
                   CELL_OK);
        }
    }
    expect("sys_mmapper_free_address(0)", sys_mmapper_free_address(0), LV2_INVALID_ARGUMENT);
    sys_addr_t map_addr = 0;
    expect("sys_mmapper_search_and_map(key=0)",
           sys_mmapper_search_and_map(0, 0, 0, &map_addr), LV2_INVALID_ARGUMENT);
    expect("sys_mmapper_change_address_access_right",
           sys_mmapper_change_address_access_right(0, 0), CELL_OK);
    expect("sys_mmapper_enable_page_fault_notification",
           sys_mmapper_enable_page_fault_notification(0, 0), LV2_INVALID_ARGUMENT);
    printf("result: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
