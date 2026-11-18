#ifndef COMBINED_AUTO_RESOURCE_CONFIG_H
#define COMBINED_AUTO_RESOURCE_CONFIG_H
#include "roll_combined2.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pfm2_config resource_config(void)
{
    pfm2_config c={{16,16,32,64,8,8,8,32,64,64,32,32,32,64,131072,262144,UINT64_C(1)<<28},
        8,8,32,8,192,64,64,64,64,4096,131072};
    return c;
}
static void resource_override(pfm2_config *c)
{
    const char *field=getenv("PFM2_AUTO_FIELD"),*value=getenv("PFM2_AUTO_VALUE");char *end;unsigned long long n;
    if(!field&&!value)return;
    if(!field||!value||!*value||strspn(value,"0123456789")!=strlen(value)){fputs("bad AUTO resource override\n",stderr);exit(2);}
    errno=0;n=strtoull(value,&end,10);if(errno||*end||n>UINT32_MAX){fputs("AUTO resource override range\n",stderr);exit(2);}
#define FIELD(name) if(!strcmp(field,#name)){c->name=(uint32_t)n;return;}
    FIELD(auto_final_runs) FIELD(auto_touched_runs) FIELD(auto_build_entries) FIELD(auto_clock_anchors)
    FIELD(auto_bootstrap_bytes) FIELD(auto_support_bytes)
#undef FIELD
    if(!strcmp(field,"metadata_bytes")){c->shared.metadata_bytes=(uint32_t)n;return;}
    if(!strcmp(field,"checkpoint_bytes")){c->shared.checkpoint_bytes=(uint32_t)n;return;}
    fputs("unknown AUTO resource override\n",stderr);exit(2);
}
#endif
