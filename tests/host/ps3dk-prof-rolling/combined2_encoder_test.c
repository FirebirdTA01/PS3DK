#include "combined2_encoder_config.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x,label) do {if(!(x)){fprintf(stderr,"PFM2_ASSERT[%s]: %s\n",label,#x);return 1;}}while(0)
int main(int argc,char **argv)
{
    pfm2_config c=test_config();pfm2_layout l,old;
    if(argc!=2)return 2;
    memset(&l,0xa5,sizeof l);old=l;
    if(!strcmp(argv[1],"planner")){
        CHECK(pfm2_plan(&c,&l)==PFR_OK,"bounded-planner-available");
        CHECK(l.context_bytes&&l.arena_bytes&&l.arena_alignment==8,"bounded-plan-shape");
        CHECK(l.context_bytes<=1048576&&l.arena_bytes<=1048576,"fixture-state-cap");
        CHECK(l.checkpoint_bound&&l.checkpoint_bound<=c.shared.checkpoint_bytes,"checkpoint-bound");
    }else if(!strcmp(argv[1],"overflow")){
        c.fifo_membership_runs=UINT32_MAX;
        CHECK(pfm2_plan(&c,&l)==PFR_INVALID,"overflow-refused");
        CHECK(!memcmp(&l,&old,sizeof l),"refusal-output-immutable");
    }else if(!strcmp(argv[1],"undersized")){
        c.shared.checkpoint_bytes=195;
        CHECK(pfm2_plan(&c,&l)==PFR_INVALID,"undersized-refused");
        CHECK(!memcmp(&l,&old,sizeof l),"refusal-output-immutable");
    }else return 2;
    return 0;
}
