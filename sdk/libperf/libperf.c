#include <string.h>

#include <cell/error.h>
#include <cell/perf/performance.h>

#define CELL_PERF_VERSION 0x0475U

void cellPerfStart(void) {}
void cellPerfStop(void) {}
void cellPerfDeleteAll(void) {}

uint32_t cellPerfGetVersion(void)
{
    return CELL_PERF_VERSION;
}

int cellPerfAddCBEpm(CellPerfCBEpmSetup *setup)
{
    (void)setup;
    return CELL_OK;
}

void cellPerfDeleteCBEpm(void) {}

int cellPerfGetCBEpmStatus(CellPerfCBEpmStatus *status)
{
    if (status) {
        memset(status, 0, sizeof(*status));
    }
    return CELL_OK;
}

int cellPerfCopyCBEpmTraceData(uint32_t offset)
{
    (void)offset;
    return CELL_OK;
}

int cellPerfCopyCBEpmTraceDataDma(uint32_t offset)
{
    (void)offset;
    return CELL_OK;
}

int cellPerfGetCBEpmWritePointer(uint32_t *writePointer)
{
    if (writePointer) {
        *writePointer = 0;
    }
    return CELL_OK;
}

void cellPerfInsertCBEpmBookmark(uint64_t bookmark)
{
    (void)bookmark;
}

int cellPerfReadAndResetCBEpmCounter(CellPerfCBEpmCounter *counter)
{
    if (counter) {
        memset(counter, 0, sizeof(*counter));
    }
    return CELL_OK;
}

int cellPerfChangeCBEpmCacheSize(uint64_t size)
{
    (void)size;
    return CELL_OK;
}

int cellPerfAddLv2OSTrace(uint32_t type, size_t bufSize, uint32_t mode,
                          uint32_t **buf)
{
    (void)type;
    (void)bufSize;
    (void)mode;
    if (buf) {
        *buf = 0;
    }
    return CELL_OK;
}

void cellPerfDeleteAllLv2OSTrace(void) {}

void cellPerfDeleteLv2OSTrace(uint32_t type)
{
    (void)type;
}

int cellPerfOpen(CellPerfSetup *setup)
{
    (void)setup;
    return CELL_OK;
}

int cellPerfClose(void)
{
    return CELL_OK;
}

int cellPerfEnable(void)
{
    return CELL_OK;
}

int cellPerfDisable(void)
{
    return CELL_OK;
}

int cellPerfGetStatus(CellPerfStatus *status)
{
    return cellPerfGetCBEpmStatus(status);
}

int cellPerfCopyTraceData(uint32_t offset, uint32_t size)
{
    (void)offset;
    (void)size;
    return CELL_OK;
}

void cellPerfInsertBookmark(uint64_t bookmark)
{
    cellPerfInsertCBEpmBookmark(bookmark);
}

int cellPerfReadAndResetCounter(CellPerfCounter *counter)
{
    return cellPerfReadAndResetCBEpmCounter(counter);
}

void cellPerfVerbose(uint32_t verbose)
{
    (void)verbose;
}

void _cellPerfVerboseSys(uint32_t verbose)
{
    (void)verbose;
}

int cellPerfSetCBEpmThreadName(uint32_t index, const char *name)
{
    (void)index;
    (void)name;
    return CELL_OK;
}
