/*
 * hello-ppu-search — cellSearch content-search smoke test.
 *
 * Exercises the init / search-start / finalize lifecycle.
 * RPCS3 HLE returns CELL_OK for most calls; this sample
 * verifies the archive links and the entry-point surface
 * is reachable.
 */

#include <cstdio>
#include <cstdint>

#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/sysutil_search.h>
#include <cell/sysutil.h>
#include <sys/timer.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* cellSearchInitialize callback must be non-NULL. */
static void search_cb(
    CellSearchEvent event, int result, const void *param, void *userData)
{
    (void)event; (void)result; (void)param; (void)userData;
}

int main(void)
{
    int32_t rc;
    int failures = 0;

    printf("[hello-ppu-search] starting\n");

    /* Load base sysutil module. */
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL);
    printf("[hello-ppu-search] cellSysmoduleLoadModule(SYSUTIL) = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL) {
        printf("[hello-ppu-search] FAIL: sysutil module load\n");
        return 1;
    }

    /* cellSearchInitialize — pass a real callback + default container. */
    rc = cellSearchInitialize(CELL_SEARCH_MODE_NORMAL, 0 /* default container */,
                              search_cb, NULL);
    printf("[hello-ppu-search] cellSearchInitialize = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* cellSearchInitialize is asynchronous — the ready state is
     * signalled via a deferred sysutil callback that fires on
     * cellSysutilCheckCallback().  Pump the queue for 10 ticks
     * (50ms each) to let the INITIALIZE_RESULT event land. */
    for (int i = 0; i < 10; i++) {
        sys_timer_usleep(50 * 1000);
        cellSysutilCheckCallback();
    }

    /* cellSearchFinalize — shut down the search utility. */
    rc = cellSearchFinalize();
    printf("[hello-ppu-search] cellSearchFinalize = %08x\n", (unsigned)rc);
    if (rc) failures++;

    if (failures) {
        printf("[hello-ppu-search] FAILED: %d calls returned non-zero\n", failures);
        return 1;
    }

    printf("[hello-ppu-search] all tests passed\n");
    return 0;
}
