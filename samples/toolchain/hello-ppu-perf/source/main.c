#include <stdio.h>

#include <sys/process.h>
#include <cell/perf/performance.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    printf("cellPerfGetVersion -> 0x%04x\n", (unsigned)cellPerfGetVersion());

    cellPerfStart();
    cellPerfInsertCBEpmBookmark(0x1000);
    cellPerfStop();
    cellPerfDeleteAll();

    return 0;
}
