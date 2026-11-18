/* Test-only capacity overrides around the unchanged relocation/immutability
 * driver. No checkpoint input or encoder internals enter this fixture. */
#include "roll_combined.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static pfm_config test_config(void)
{
    pfm_config c={16,16,32,64,8,8,8,32,128,64,128,128,32,256,65552,131072,4194304};
    const char *field=getenv("PFR_TEST_FIELD"),*value=getenv("PFR_TEST_VALUE");char *end;unsigned long long n;
    if(!field&&!value)return c;
    if(!field||!value||!*value||strspn(value,"0123456789")!=strlen(value)){fputs("bad test override\n",stderr);exit(2);}
    errno=0;n=strtoull(value,&end,10);
    if(errno||*end||n>UINT32_MAX){fputs("test override range\n",stderr);exit(2);}
#define SET(name) if(!strcmp(field,#name)){c.name=(uint32_t)n;return c;}
    SET(spu_domains) SET(spu_lanes) SET(spu_images) SET(epochs) SET(brackets) SET(image_descriptors)
    SET(journal_entries) SET(event_runs) SET(live_spu_stack_rows) SET(saved_spu_witnesses)
    SET(metadata_bytes) SET(prepare_work)
#undef SET
    fputs("unknown test override\n",stderr);exit(2);
}
#define COMBINED_ENCODER_TEST_CONFIG_H
#include "combined_encoder_fixture.c"
