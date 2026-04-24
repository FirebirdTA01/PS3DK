/*
 * hello-ppu-spurs-semaphore - PPU-visible SPURS semaphore bring-up.
 *
 * Counting semaphore smoke test.  Initialize with total=3, read back
 * the taskset-address via GetTasksetAddress (returns null for IWL-
 * initialized semaphores, which is fine - the call still has to reach
 * the SPRX), tear down.  Actual P/V lives on the SPU side; this
 * validates our header surface + NID resolution + SPRX init path.
 */

#include <cstdio>
#include <cstring>

#include <sys/process.h>
#include <sys/spu_thread_group.h>
#include <cell/spurs.h>
#include <spu_printf.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    std::printf("hello-ppu-spurs-semaphore\n");

    int rc = spu_printf_initialize(999, 0);
    if (rc) { std::printf("  spu_printf_initialize: %#x\n", rc); return 1; }

    cell::Spurs::SpursAttribute attr;
    cell::Spurs::SpursAttribute::initialize(&attr, 4, 100, 2, false);
    attr.setNamePrefix("Sem", 3);
    attr.setSpuThreadGroupType(SYS_SPU_THREAD_GROUP_TYPE_EXCLUSIVE_NON_CONTEXT);
    attr.enableSpuPrintfIfAvailable();

    cell::Spurs::Spurs2 *spurs = new cell::Spurs::Spurs2;
    rc = cell::Spurs::Spurs2::initialize(spurs, &attr);
    if (rc) { std::printf("  Spurs2::initialize: %#x\n", rc); return 1; }
    std::printf("  Spurs2 up\n");

    CellSpursSemaphore sem;
    std::memset(&sem, 0, sizeof(sem));

    rc = cellSpursSemaphoreInitializeIWL(spurs, &sem, /*total=*/3);
    if (rc) { std::printf("  SemaphoreInitializeIWL: %#x\n", rc); goto teardown; }
    std::printf("  Semaphore init ok (total=3)\n");

    {
        CellSpursTaskset *ts = 0;
        rc = cellSpursSemaphoreGetTasksetAddress(&sem, &ts);
        std::printf("  GetTasksetAddress rc=%#x taskset=%p\n", rc, (void *)ts);
    }

teardown:
    rc = spurs->finalize();
    std::printf("  Spurs finalize: %#x\n", rc);
    delete spurs;
    spu_printf_finalize();

    std::printf("hello-ppu-spurs-semaphore: done\n");
    return 0;
}
