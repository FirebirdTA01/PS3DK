#include "auto_wait_model.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static awm_model m;
static uint64_t begin(unsigned reason,unsigned domain,uint64_t parent,uint64_t tb) {
    return awm_begin(&m,1,1,reason,domain,parent,1,0,tb,200,42);
}
int main(void) {
    uint64_t total=999,p=begin(1,AWM_CPU_POLL,0,100);
    if(!p){fputs("Finish begin missing\n",stderr);return 1;}
    uint64_t child=begin(2,AWM_CPU_POLL,p,110);
    assert(child && awm_end(&m,1,child,120,AWM_SUCCESS));
    child=begin(3,AWM_CPU_POLL,p,120);
    assert(child && awm_end(&m,1,child,150,AWM_SUCCESS));
    assert(awm_end(&m,1,p,160,AWM_SUCCESS));
    assert(awm_cpu_total(&m,&total) && total==60); /* not 60+10+30 */
    assert(m.records[0].owner==1 && m.records[0].frame==0 && m.records[1].parent==p);
    assert(m.records[1].target==200 && m.records[1].value==42);
    memset(&m,0,sizeof m);p=begin(1,AWM_CPU_POLL,0,100);child=begin(8,AWM_INTERNAL,p,110);
    assert(awm_end(&m,1,child,150,AWM_SUCCESS) && awm_end(&m,1,p,160,AWM_SUCCESS));
    if(!awm_cpu_total(&m,&total) || total!=20){fputs("nested observer overhead entered application total\n",stderr);return 1;}
    memset(&m,0,sizeof m);p=begin(4,AWM_GPU_COMMAND,0,100);
    assert(p && awm_end(&m,1,p,300,AWM_SUCCESS));
    assert(!m.records[0].valid && m.commands==1);
    assert(awm_cpu_total(&m,&total) && total==0);
    p=begin(5,AWM_INTERNAL,0,400);assert(p && awm_end(&m,1,p,500,AWM_SUCCESS));
    assert(m.internal==1 && m.records[1].duration==100);
    assert(awm_cpu_total(&m,&total) && total==0);
    memset(&m,0,sizeof m);p=begin(6,AWM_CPU_CALL_OPAQUE,0,100);
    assert(p && !awm_end(&m,2,p,200,AWM_SUCCESS));
    assert(awm_end(&m,1,p,200,AWM_ERROR));assert(!awm_cpu_total(&m,&total));
    memset(&m,0,sizeof m);p=begin(7,AWM_CPU_POLL,0,100);
    assert(awm_end(&m,1,p,99,AWM_SUCCESS));assert(m.records[0].outcome==AWM_CLOCK && !m.records[0].valid);
    assert(!awm_cpu_total(&m,&total));
    memset(&m,0,sizeof m);p=begin(7,AWM_CPU_POLL,0,100);
    assert(awm_end(&m,1,p,200,AWM_DEADLINE));assert(m.records[0].outcome==AWM_DEADLINE && m.records[0].duration==100);
    memset(&m,0,sizeof m);assert(!awm_end(&m,1,77,200,AWM_SUCCESS));
    assert(m.orphan==1 && m.loss && m.used==0 && !awm_cpu_total(&m,&total));
    memset(&m,0,sizeof m);p=begin(1,AWM_CPU_POLL,0,100);awm_stop(&m);
    assert(m.records[0].outcome==AWM_STOPPED && !m.records[0].valid && !awm_cpu_total(&m,&total));
    assert(!begin(1,AWM_CPU_POLL,0,200));
    memset(&m,0,sizeof m);p=0;
    for(unsigned i=0;i<AWM_DEPTH;i++){p=begin(1,AWM_CPU_POLL,p,100+i);assert(p);}
    assert(!begin(1,AWM_CPU_POLL,p,200) && m.depth==AWM_DEPTH && m.loss);
    assert(!awm_end(&m,1,p+1,201,AWM_SUCCESS) && m.depth==AWM_DEPTH);
    memset(&m,0,sizeof m);m.next_id=UINT64_MAX;assert(!begin(1,AWM_CPU_POLL,0,100) && m.loss);
    memset(&m,0,sizeof m);
    for(unsigned i=0;i<AWM_RECORDS;i++){p=begin(1,AWM_CPU_POLL,0,i*2);assert(p && awm_end(&m,1,p,i*2+1,AWM_SUCCESS));}
    assert(!begin(1,AWM_CPU_POLL,0,100) && m.used==AWM_RECORDS && m.loss);
    memset(&m,0,sizeof m);p=begin(6,AWM_CPU_CALL_OPAQUE,0,100);
    assert(awm_end(&m,1,p,150,AWM_ERROR) && awm_cpu_total(&m,&total) && total==50);
    memset(&m,0,sizeof m);p=begin(8,AWM_INTERNAL,0,100);child=begin(1,AWM_CPU_POLL,p,110);
    assert(awm_end(&m,1,child,120,AWM_SUCCESS) && awm_end(&m,1,p,130,AWM_SUCCESS));
    assert(awm_cpu_total(&m,&total) && total==0 && m.internal==1);
    memset(&m,0,sizeof m);p=begin(1,AWM_CPU_POLL,0,100);
    assert(!awm_cpu_total(&m,&total) && m.records[0].outcome==AWM_OPEN);
    assert(!begin(1,AWM_CPU_POLL,0,110) && m.depth==1);
    memset(&m,0,sizeof m);assert(!begin(1,0,0,100) && m.used==0);
    memset(&m,0,sizeof m);p=awm_begin(&m,1,1,1,AWM_CPU_POLL,0,0,0,100,0,0);
    assert(p && m.records[0].owner==0 && m.records[0].frame==0);
    printf("wait nesting/domain/open/loss/clock/bounds PASS\n");return 0;
}
