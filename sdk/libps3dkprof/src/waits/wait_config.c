#include "wait_internal.h"
#include <string.h>
/* Zero size means defaults, resolved against the core thread ceiling at init. */
ps3dk_prof_wait_config p_wait_options;
int p_wait_layout(ps3dk_prof_wait_config c,uint32_t *bytes)
{
    uint64_t n;
    if(c.size!=sizeof c || !c.max_actors || c.max_actors>1024 ||
       c.ring_records<8 || c.ring_records>4096 || (c.ring_records&(c.ring_records-1)) ||
       !c.max_bytes || c.max_bytes>8388608)return PS3DK_PROF_EINVAL;
    n=W_CAPTURE+(uint64_t)c.max_actors*(W_CONTROL+(uint64_t)c.ring_records*W_REC);
    if(n>c.max_bytes)return PS3DK_PROF_EINVAL;
    *bytes=(uint32_t)n;return 0;
}
int ps3dk_prof_wait_configure(const ps3dk_prof_wait_config *cfg)
{
    uint32_t expected=G_UNINIT,bytes;int rc;
    if(!CAS_ACQREL(&g_gate,&expected,G_CONFIGURING))
        return expected==G_READY || expected==G_TERMINAL ? PS3DK_PROF_EALREADY : PS3DK_PROF_EBUSY;
    rc=cfg?p_wait_layout(*cfg,&bytes):PS3DK_PROF_EINVAL;
    if(!rc)p_wait_options=*cfg;
    ST_REL(&g_gate,G_UNINIT);return rc;
}
int p_wait_init(prof *p)
{
    ps3dk_prof_wait_config c=p_wait_options;uint32_t bytes;
    if(!c.size){c.size=sizeof c;c.max_bytes=1048576;c.max_actors=p->nslots<64?p->nslots:64;c.ring_records=128;}
    if(!c.max_actors)c.max_actors=p->nslots<64?p->nslots:64;
    if(c.max_actors>p->nslots || p_wait_layout(c,&bytes))return PS3DK_PROF_EINVAL;
    p->waits=pal_alloc(bytes);if(!p->waits)return PS3DK_PROF_ENOMEM;
    memset(p->waits,0,bytes);p->waits->cfg=c;p->waits->allocated=bytes;
    p->waits->stride=W_CONTROL+c.ring_records*W_REC;return 0;
}
void p_wait_unwind(prof *p){if(p->waits){pal_free(p->waits);p->waits=NULL;}}
wait_actor *p_wait_actor(wait_capture *w,uint32_t inc)
{
    if(!inc || inc>w->cfg.max_actors)return NULL;
    return (wait_actor *)(void *)((uint8_t *)w+W_CAPTURE+(size_t)(inc-1)*w->stride);
}
uint8_t *p_wait_record(wait_capture *w,wait_actor *a,uint64_t pos)
{
    return (uint8_t *)a+W_CONTROL+(size_t)(pos&(w->cfg.ring_records-1))*W_REC;
}
