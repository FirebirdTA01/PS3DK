/* Configure-only member in each observed archive. No lifecycle ownership. */
#include "wait_internal.h"

void __ps3dk_prof_wait_config_anchor(void) {}

__attribute__((constructor(99))) static void wait_defaults(void)
{
    uint32_t expected=G_UNINIT;
    if(!CAS_ACQREL(&g_gate,&expected,G_CONFIGURING))return;
    /* Public configuration refuses max_actors=0. This private default sentinel
     * is resolved against the actual core thread ceiling at initialization. */
    if(!p_wait_options.size){
        p_wait_options.size=sizeof p_wait_options;
        p_wait_options.max_bytes=1048576;
        p_wait_options.max_actors=0;
        p_wait_options.ring_records=128;
    }
    ST_REL(&g_gate,G_UNINIT);
}
