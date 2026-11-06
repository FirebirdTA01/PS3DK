#include "../source/workload.h"
#include <stdio.h>
static unsigned tests, failures;
static void check(int yes, const char *name)
{
    ++tests;
    if (!yes) { ++failures; printf("FAIL %s\n", name); }
}
int main(void)
{
    /* Golden values from an independent arbitrary-precision Python oracle. */
    check(work_mix(0,128)==0x919fa3fbu,"golden-zero");
    check(work_mix(1,128)==0xdaa5cb77u,"golden-one");
    check(work_mix(16383,128)==0xff45a84eu,"golden-last");
    for (unsigned n=1;n<=6;++n) {
        uint32_t count=0,digest=0,lo=WORK_ITEMS,hi=0;
        for(unsigned i=0;i<n;++i) {
            work_result r=work_partition(i,n,WORK_ITEMS);
            count+=r.count; digest^=r.digest;
            if(r.count<lo)lo=r.count;
            if(r.count>hi)hi=r.count;
        }
        check(count==WORK_ITEMS,"partition-coverage");
        check(digest==WORK_DIGEST,"partition-golden-digest");
        check(hi-lo<=1,"partition-balance");
    }
    for(unsigned n=1;n<=6;++n) {
        uint32_t count=0,digest=0;
        for(unsigned i=0;i<n;++i){work_result r=work_partition(i,n,7);count+=r.count;digest^=r.digest;}
        check(count==7 && digest==work_partition(0,1,7).digest,"uneven-small-partition");
    }
    check(work_partition(0,0,10).count==0,"invalid-zero-lanes");
    check(work_partition(6,6,10).count==0,"invalid-lane");
    check(work_partition(0,7,10).count==0,"invalid-many-lanes");
    check(work_partition(0,1,0).count==0,"empty");
    check(work_phase(0,100)==0,"phase-start");
    check(work_phase(99,100)==0,"phase-before-boundary");
    check(work_phase(100,100)==1,"phase-boundary");
    check(work_phase(299,100)==2,"phase-heavy-spu");
    check(work_phase(300,100)==3,"phase-heavy-ppu");
    check(work_phase(400,100)==0,"phase-loop");
    check(work_phase(UINT64_MAX,100)==(int)((UINT64_MAX/100)%4),"phase-wide-time");
    check(work_phase(0,0)==-1,"phase-invalid-duration");
    printf("workload tests=%u pass=%u fail=%u\n",tests,tests-failures,failures);
    return failures ? 1 : 0;
}
