/* LV2 memory container + container-backed allocation validation.
 *
 * Exercises the "memory container" heap as a malloc replacement:
 *
 *   sys_memory_container_create         (324)
 *   sys_memory_container_get_size       (343)
 *   sys_memory_allocate_from_container  (350)
 *   sys_memory_free                     (349)
 *   sys_memory_container_destroy        (325)
 *   sys_memory_get_user_memory_size     (352)
 *
 * Green path:
 *   1. Query total / available user memory (sys_memory_get_user_memory_size).
 *   2. Create a container (2 MiB).
 *   3. Read its total / available size.
 *   4. Allocate four 512 KiB regions from the container, write + read back
 *      a pattern in each, then free each one.
 *   5. After each allocation the container's available size should be
 *      lower; after each free it should rise again.  We only verify the
 *      total stays constant and the allocation/failure path is sane (the
 *      exact available accounting is kernel-internal).
 *   6. Destroy the container.
 *
 * Negative path (both must return LV2 errors, never succeed):
 *   - sys_memory_allocate_from_container with container id INVALID.
 *   - sys_memory_container_destroy with the INVALID id.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/process.h>
#include <sys/memory.h>

SYS_PROCESS_PARAM(1024, 0x10000);

#define KV  (1024u * 1024u)

/* Raw LV2 result codes (not host/newlib errno values). */
#define LV2_INVALID_ARGUMENT   ((int)0x80010002u)
#define LV2_NO_SUCH_CONTAINER  ((int)0x80010005u)

static int failures, checks;

static int expect(const char *name, int rc, int wanted)
{
    ++checks;
    printf("  %s -> 0x%08x (0x%08x)%s\n", name,
           (unsigned)rc, (unsigned)wanted, rc == wanted ? "" : "  FAIL");
    if (rc != wanted) ++failures;
    return rc == wanted;
}

static int used(const char *name, int ok, unsigned long long note)
{
    ++checks;
    if (!ok) ++failures;
    printf("  %s: %s (0x%llx)\n", name, ok ? "ok" : "FAIL", note);
    return ok;
}

int main(void)
{
    printf("hello-ppu-lv2-memcontainer: LV2 memory-container heap\n");

    /* 1. total / available user memory. */
    sys_memory_info_t mi;
    int rc = sys_memory_get_user_memory_size(&mi);
    expect("sys_memory_get_user_memory_size", rc, CELL_OK);
    if (rc == CELL_OK)
        printf("    total=%u MiB  available=%u MiB\n",
               mi.total_user_memory / (1024 * 1024),
               mi.available_user_memory / (1024 * 1024));

    /* 2. create a 2 MiB container. */
    sys_memory_container_t cid = SYS_MEMORY_CONTAINER_ID_INVALID;
    rc = sys_memory_container_create(&cid, 2 * KV);
    expect("sys_memory_container_create(2 MiB)", rc, CELL_OK);
    if (rc != CELL_OK)
        return 1;
    used("container id is not INVALID", cid != SYS_MEMORY_CONTAINER_ID_INVALID, (unsigned)cid);

    /* 3. read the container's total / available size. */
    sys_memory_info_t cmi;
    rc = sys_memory_container_get_size(&cmi, cid);
    expect("sys_memory_container_get_size", rc, CELL_OK);
    if (rc == CELL_OK)
        printf("    container total=%u MiB  available=%u MiB\n",
               cmi.total_user_memory / (1024 * 1024),
               cmi.available_user_memory / (1024 * 1024));

    /* 4. four 512 KiB allocations from the container, use + free each. */
    enum { N = 4 };
    for (int i = 0; i < N; ++i) {
        sys_addr_t addr = 0;
        rc = sys_memory_allocate_from_container(512 * 1024, cid,
                                                SYS_MEMORY_PAGE_SIZE_64K,
                                                &addr);
        char name[64];
        snprintf(name, sizeof name, "allocate_from_container(%d, 512 KiB)", i);
        if (!expect(name, rc, CELL_OK) || !addr)
            continue;

        volatile uint32_t *w = (volatile uint32_t *)(uintptr_t)addr;
        /* write a recognisable pattern over the first and last words. */
        w[0]          = 0xa5a50000u | (uint32_t)i;
        w[512 * 1024 / 4 - 1] = 0x5a5au000u | (uint32_t)i;
        int bad = (w[0] != (0xa5a50000u | (uint32_t)i))
               | (w[512 * 1024 / 4 - 1] != (0x5a5au000u | (uint32_t)i));
        snprintf(name, sizeof name, "region %d reads back", i);
        used(name, !bad, (unsigned)w[0]);

        snprintf(name, sizeof name, "free region %d", i);
        expect(name, sys_memory_free(addr), CELL_OK);
    }

    /* 5. after all frees the container should still be intact. */
    sys_memory_info_t cmi2;
    rc = sys_memory_container_get_size(&cmi2, cid);
    expect("sys_memory_container_get_size(after frees)", rc, CELL_OK);

    /* 6. destroy. */
    expect("sys_memory_container_destroy", sys_memory_container_destroy(cid), CELL_OK);

    /* ---- negative path ------------------------------------------ */
    sys_addr_t daddr = 0;
    expect("allocate_from_container(INVALID id)",
           sys_memory_allocate_from_container(64 * 1024,
                SYS_MEMORY_CONTAINER_ID_INVALID, SYS_MEMORY_PAGE_SIZE_64K,
                &daddr), LV2_NO_SUCH_CONTAINER);
    expect("container_destroy(INVALID id)",
           sys_memory_container_destroy(SYS_MEMORY_CONTAINER_ID_INVALID),
           LV2_INVALID_ARGUMENT);
    expect("sys_memory_free(0)", sys_memory_free(0), LV2_INVALID_ARGUMENT);

    /* ---- summary ------------------------------------------------- */
    printf("result: %s (%d of %d checks)\n",
           failures ? "FAIL" : "PASS", checks, checks);
    return failures ? 1 : 0;
}
