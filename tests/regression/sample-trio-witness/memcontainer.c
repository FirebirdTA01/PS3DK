/*
 * hello-ppu-lv2-memcontainer witness.
 *
 *   case 0 control: real system calls; the sample must pass.
 *   case 1 ok-null: allocations from the sample's container return
 *          CELL_OK with address 0; the sample must fail.
 */
#include <stdint.h>
#include <sys/process.h>
#include <sys/memory.h>

#include "witness.h"

static int fake_allocs;

static int fake_allocate_from_container(uint32_t size, sys_memory_container_t cid,
                                        uint64_t flags, sys_addr_t *alloc_addr)
{
    if (WITNESS_CASE == 1 && cid != SYS_MEMORY_CONTAINER_ID_INVALID) {
        fake_allocs++;
        *alloc_addr = 0;
        return CELL_OK;
    }
    return sys_memory_allocate_from_container(size, cid, flags, alloc_addr);
}

#define sys_memory_allocate_from_container fake_allocate_from_container
#define main sample_main
#include "lv2/hello-ppu-lv2-memcontainer/source/main.c"
#undef main
#undef sys_memory_allocate_from_container

int main(void)
{
    int rc = sample_main();
    printf("witness %s: sample exit status %d\n", WITNESS_NAME, rc);
    if (WITNESS_CASE == 0) {
        WITNESS_REQUIRE(rc == 0);
    } else {
        WITNESS_REQUIRE(rc == 1);
        WITNESS_REQUIRE(fake_allocs == 4);
    }
    return witness_finish();
}
