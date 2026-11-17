/* Private schema2 writer; no runtime/public installation.
 * Shared validators live in roll_combined.c; this owner adds FIFO/43 and AUTO. */
#include "roll_combined2.h"
#include "roll_combined_internal.h"
#include <string.h>
#define F2_MAGIC UINT32_C(0x50464d32)
#define F2_SIDE UINT32_C(0x50464632)
static uint16_t r16(const uint8_t *p){return (uint16_t)((unsigned)p[0]|(unsigned)p[1]<<8);}
static uint32_t r32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t r64(const uint8_t *p){return (uint64_t)r32(p)|(uint64_t)r32(p+4)<<32;}
static void w16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void w32(uint8_t *p,uint32_t n){unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void w64(uint8_t *p,uint64_t n){w32(p,(uint32_t)n);w32(p+4,(uint32_t)(n>>32));}
static uint32_t crc(const uint8_t *p,size_t n)
{uint32_t c=UINT32_MAX;unsigned i;while(n--){c^=*p++;for(i=0;i<8;++i)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}return ~c;}
static int plus64(uint64_t *n,uint64_t v){if(v>UINT64_MAX-*n)return 0;*n+=v;return 1;}
static int times64(uint64_t *n,uint64_t count,uint64_t size)
{if(count&&size>UINT64_MAX/count)return 0;return plus64(n,count*size);}
static int fits32(size_t n)
{
#if SIZE_MAX > UINT32_MAX
    return n<=UINT32_MAX;
#else
    (void)n;return 1;
#endif
}
static int partition(size_t *n,size_t count,size_t size,uint32_t *at)
{
    size_t pad=(8-*n%8)%8;
    if(pad>SIZE_MAX-*n)return 0;
    *n+=pad;if(!fits32(*n))return 0;
    *at=(uint32_t)*n;
    if(count&&size>SIZE_MAX/count)return 0;
    if(count*size>SIZE_MAX-*n)return 0;
    *n+=count*size;return fits32(*n);
}
#include "roll_combined2_fifo.h"
#include "roll_combined2_auto.h"
typedef struct {uint32_t magic,windows;fi_state fifo;au_state automatic;} side;
typedef struct {
    uint32_t magic,core_offset,side_offset,runs_offset,groups_offset,reserved;
    uint32_t auto_finals_offset,auto_touched_offset,auto_builds_offset,auto_clocks_offset;
    pfm2_config cfg;pfm2_layout layout;pfm_layout core_layout;pfm_core_config core_cfg;
} context;
_Static_assert(_Alignof(context)<=8&&_Alignof(side)<=8&&_Alignof(fi_run)<=8&&_Alignof(fi_group)<=8
    &&_Alignof(au_build)<=8&&_Alignof(au_clock)<=8,"schema2 arena alignment");
static void *core(const context *c){return (uint8_t *)(void *)c+c->core_offset;}
static side *state(const context *c,const void *a){return (side *)((uint8_t *)(void *)a+c->side_offset);}
static fi_run *runs(const context *c,const void *a){return (fi_run *)((uint8_t *)(void *)a+c->runs_offset);}
static fi_group *groups(const context *c,const void *a){return (fi_group *)((uint8_t *)(void *)a+c->groups_offset);}
static au_pools auto_pools(const context *c,const void *a)
{
    uint8_t *b=(uint8_t *)(void *)a;au_pools p;
    p.finals=(fi_run *)(void *)(b+c->auto_finals_offset);p.touched=(fi_run *)(void *)(b+c->auto_touched_offset);
    p.builds=(au_build *)(void *)(b+c->auto_builds_offset);p.clocks=(au_clock *)(void *)(b+c->auto_clocks_offset);return p;
}
static int plan(const pfm2_config *cfg,context *c)
{
    size_t n;uint32_t end;uint64_t bound;
    if(!cfg||!cfg->shared.prepare_work||cfg->rsx_domains>64
       ||(cfg->family_flags!=64&&cfg->family_flags!=128&&cfg->family_flags!=192))return PFR_INVALID;
    if(!(cfg->family_flags&128)&&(cfg->auto_final_runs||cfg->auto_touched_runs||cfg->auto_build_entries
       ||cfg->auto_clock_anchors||cfg->auto_bootstrap_bytes||cfg->auto_support_bytes))return PFR_INVALID;
    if(!(cfg->family_flags&64)&&(cfg->fifo_membership_runs||cfg->accounting_groups))return PFR_INVALID;
    memset(c,0,sizeof *c);c->cfg=*cfg;c->core_cfg.base=cfg->shared;
    c->core_cfg.family_flags=cfg->family_flags;c->core_cfg.rsx_domains=cfg->rsx_domains;
    if(pfm_core_plan(&c->core_cfg,&c->core_layout)!=PFR_OK)return PFR_INVALID;
    bound=c->core_layout.checkpoint_bound;
    if(!plus64(&bound,48))return PFR_INVALID;
    if((cfg->family_flags&64)&&(!plus64(&bound,64+404)||!times64(&bound,cfg->fifo_membership_runs,24)
       ||!times64(&bound,cfg->accounting_groups,32)))return PFR_INVALID;
    if((cfg->family_flags&128)&&(!plus64(&bound,32+532+216+208)||!plus64(&bound,cfg->auto_bootstrap_bytes)
       ||!times64(&bound,(uint64_t)cfg->auto_final_runs+cfg->auto_touched_runs,24)
       ||!times64(&bound,cfg->auto_build_entries,608)||!times64(&bound,cfg->auto_clock_anchors,104)))return PFR_INVALID;
    if(bound>UINT32_MAX||bound>cfg->shared.checkpoint_bytes)return PFR_INVALID;
    n=sizeof *c;if(!partition(&n,1,c->core_layout.context_bytes,&c->core_offset)
       ||!partition(&n,0,1,&end))return PFR_INVALID;
    c->layout.context_bytes=n;
    n=c->core_layout.arena_bytes;
    if(!partition(&n,1,sizeof(side),&c->side_offset)
       ||!partition(&n,cfg->fifo_membership_runs,sizeof(fi_run),&c->runs_offset)
       ||!partition(&n,cfg->accounting_groups,sizeof(fi_group),&c->groups_offset)
       ||!partition(&n,cfg->auto_final_runs,sizeof(fi_run),&c->auto_finals_offset)
       ||!partition(&n,cfg->auto_touched_runs,sizeof(fi_run),&c->auto_touched_offset)
       ||!partition(&n,cfg->auto_build_entries,sizeof(au_build),&c->auto_builds_offset)
       ||!partition(&n,cfg->auto_clock_anchors,sizeof(au_clock),&c->auto_clocks_offset)
       ||!partition(&n,0,1,&end))return PFR_INVALID;
    c->layout.arena_bytes=n;c->layout.arena_alignment=8;c->layout.checkpoint_bound=(size_t)bound;c->magic=F2_MAGIC;return PFR_OK;
}
int pfm2_plan(const pfm2_config *cfg,pfm2_layout *out)
{context c;int rc;if(!out)return PFR_INVALID;rc=plan(cfg,&c);if(rc==PFR_OK)*out=c.layout;return rc;}
static int admit(const uint8_t *h,uint32_t families)
{
    uint32_t f,base;if(!h||memcmp(h,"PS3DKPRF",8)||r16(h+8)!=1)return 0;
    f=r32(h+12);base=f&7;
    return (f&192)==families&&!(f&~UINT32_C(711))&&(base==1||base==3||base==7);
}
static void section(uint8_t *b,uint16_t kind,uint32_t count,uint32_t n)
{w16(b,kind);w16(b+2,1);w32(b+4,count);w32(b+8,n);w32(b+12,0);}
static int encode(const context *c,const void *a,void *output,size_t capacity,size_t *written)
{
    pfm_core_view v;const side *s=state(c,a);const fi_state *f=&s->fifo;const fi_group *g=groups(c,a);
    uint8_t *b=output;uint64_t length,expected_chunks,expected_bytes,old_status;
    const au_state *au=&s->automatic;size_t at,common=0,inner,auto_inner=0;uint32_t i;int rc;
    int has_fifo=(c->cfg.family_flags&64)!=0,has_auto=(c->cfg.family_flags&128)!=0;
    if(s->magic!=F2_SIDE||f->run_count>c->cfg.fifo_membership_runs||f->group_count>c->cfg.accounting_groups
       ||s->windows>c->cfg.windows||pfm_core_snapshot(core(c),a,&v)!=PFR_OK)return PFR_INVALID;
    inner=has_fifo?fi_wire_size(f):0;
    if(has_auto){rc=au_size(au,auto_pools(c,a),&c->cfg,&v.ppu,&auto_inner);if(rc)return rc;}
    length=96+16+(uint64_t)v.ppu.metadata_bytes+v.sections_bytes+16+16+4;
    if(has_fifo)length+=64+(uint64_t)inner+(uint64_t)f->group_count*32;
    if(has_auto)length+=32+(uint64_t)auto_inner;
    if(length>capacity||length>c->cfg.shared.checkpoint_bytes)return PFR_CHECKPOINT_BYTES;
    old_status=f->statuses?f->statuses-1:0;
    expected_chunks=v.ppu.metadata_count;
    if(!plus64(&expected_chunks,v.ppu_record_chunks)||!plus64(&expected_chunks,v.spu_event_chunks)
       ||!plus64(&expected_chunks,f->event_chunks)||!plus64(&expected_chunks,old_status)
       ||!plus64(&expected_chunks,f->parsed)||!plus64(&expected_chunks,au->op_chunks)
       ||!plus64(&expected_chunks,au->statuses?au->statuses-1:0))return PFR_COUNTER_RANGE;
    if(v.ppu.metadata_bytes<(uint64_t)v.ppu.metadata_count*4)return PFR_INVALID;
    expected_bytes=v.ppu.metadata_bytes-(uint64_t)v.ppu.metadata_count*4;
    if(!times64(&expected_bytes,v.ppu_record_chunks,28)||!times64(&expected_bytes,v.ppu.physical_rows,32)
       ||!times64(&expected_bytes,v.spu_event_chunks,40)||!times64(&expected_bytes,v.spu_events,8)
       ||!times64(&expected_bytes,f->event_chunks,36)||!times64(&expected_bytes,f->events,96)
       ||!times64(&expected_bytes,old_status,100)||!times64(&expected_bytes,f->parsed,100)
       ||!times64(&expected_bytes,au->op_chunks,36)||!times64(&expected_bytes,au->ops,au_width(au))
       ||!times64(&expected_bytes,au->statuses?au->statuses-1:0,au_ringed(au)?228:148))return PFR_COUNTER_RANGE;
    if(expected_chunks!=v.ppu.next||expected_bytes!=v.ppu.bytes)return PFR_INVALID;
    memset(b,0,96);memcpy(b,"PFRMIX01",8);w16(b+8,2);w16(b+10,96);w32(b+12,6);memcpy(b+16,v.ppu.header,16);
    w64(b+32,v.ppu.next);w64(b+40,v.ppu.bytes);w64(b+48,length-100);w32(b+56,3|(has_auto?4:0)|(has_fifo?8:0));
    for(i=0;i<6;++i)w16(b+64+2*i,1);
    section(b+96,1,v.ppu.metadata_count,v.ppu.metadata_bytes);memcpy(b+112,v.ppu.metadata,v.ppu.metadata_bytes);at=112+v.ppu.metadata_bytes;
    rc=pfm_core_sections(core(c),a,b+at,capacity-at,&common);if(rc!=PFR_OK)return rc;
    if(common!=v.sections_bytes)return PFR_INVALID;
    at+=common;
    section(b+at,5,(uint32_t)has_auto,has_auto?(uint32_t)(32+auto_inner):0);at+=16;
    if(has_auto){memset(b+at,0,32);w32(b+at,(uint32_t)auto_inner);w64(b+at+8,au->status_chunks);at+=32;
        rc=au_encode(au,auto_pools(c,a),&c->cfg,&v.ppu,b+at,auto_inner);if(rc)return rc;at+=auto_inner;}
    section(b+at,6,(uint32_t)has_fifo,has_fifo?(uint32_t)(64+inner+(size_t)f->group_count*32):0);at+=16;
    if(has_fifo){
    memset(b+at,0,64);w32(b+at,(uint32_t)inner);w32(b+at+4,f->group_count);
    w64(b+at+8,f->config_index);w64(b+at+16,f->status_index);w64(b+at+24,f->event_chunks);
    w64(b+at+32,f->status_chunks);w64(b+at+40,f->chunks);w64(b+at+48,f->bytes);at+=64;
    at+=fi_encode(f,runs(c,a),v.ppu.next,v.ppu.clock_frequency,(r32(v.ppu.header+12)&512)!=0,b+at);
    for(i=0;i<f->group_count;++i){w64(b+at,g[i].window);w64(b+at+8,g[i].rows);w64(b+at+16,g[i].first);w64(b+at+24,g[i].last);at+=32;}
    }
    if(at!=length-4)return PFR_INVALID;
    w32(b+at,crc(b,at));*written=at+4;return PFR_OK;
}
int pfm2_init(void *opaque,size_t context_bytes,const pfm2_config *cfg,const uint8_t h[16],
              void *a,size_t arena_bytes,void *cp,size_t capacity,size_t *written)
{
    context c,*out=opaque;int rc;if(written)*written=0;
    if(!opaque||!a||!cp||!written||!cfg||!admit(h,cfg->family_flags)||plan(cfg,&c)!=PFR_OK
       ||context_bytes<c.layout.context_bytes||arena_bytes<c.layout.arena_bytes||capacity<cfg->shared.checkpoint_bytes
       ||(uintptr_t)opaque%8||(uintptr_t)a%8)return PFR_INVALID;
    memset(out,0,c.layout.context_bytes);memcpy(out,&c,sizeof c);memset(a,0,c.layout.arena_bytes);
    rc=pfm_core_init(core(out),c.core_layout.context_bytes,&c.core_cfg,h,a,c.core_layout.arena_bytes);if(rc!=PFR_OK)return rc;
    state(out,a)->magic=F2_SIDE;rc=encode(out,a,cp,capacity,written);return rc==PFR_OK?PFR_OK:PFR_INVALID;
}
int pfm2_work(const void *opaque,const void *chunk,size_t n,uint64_t *core_work,uint64_t *family_work)
{
    const context *c=opaque;const uint8_t *raw=chunk;uint64_t base,extra=32,rows=0,auto_rows=0;uint32_t ty,len;int rc;
    if(!c||c->magic!=F2_MAGIC||!raw||!core_work||!family_work||n<20)return PFR_INVALID;
    ty=r32(raw);len=r32(raw+4);if(len>UINT32_C(1048576)||(uint64_t)len+20!=n)return PFR_INVALID;
    if(ty==32){if(len<16)return PFR_INVALID;rows=r32(raw+24);if(!rows||rows*96+16!=len)return PFR_INVALID;}
    if(ty==35){uint32_t width;if(len<16)return PFR_INVALID;auto_rows=r32(raw+24);width=r32(raw+28);
        if(!auto_rows||(width!=128&&width!=144&&width!=160&&width!=176)||auto_rows*width+16!=len)return PFR_INVALID;}
    if(ty==36)auto_rows=1;
    rc=pfm_core_work(core(c),chunk,n,&base);if(rc!=PFR_OK)return rc;
    /* Logical byte/row work. CRC's fixed eight bit steps are a constant factor.
     * Reserve side clone, all queries, member shifts and outer/inner emission;
     * common core reserves its clone, validators and common section encoding. */
    if(!plus64(&extra,c->layout.arena_bytes-c->core_layout.arena_bytes)
       ||!times64(&extra,c->layout.checkpoint_bound,4)||!times64(&extra,c->cfg.shared.metadata_bytes,4)
       ||!times64(&extra,c->cfg.shared.incarnations,2)||!times64(&extra,c->cfg.rsx_domains,2)
       ||!times64(&extra,n,4)||!times64(&extra,rows,32*((uint64_t)c->cfg.fifo_membership_runs+1))
       ||!times64(&extra,c->cfg.accounting_groups,32)||!times64(&extra,c->cfg.windows,32)
       ||!times64(&extra,auto_rows,1024*((uint64_t)c->cfg.auto_final_runs+c->cfg.auto_touched_runs+c->cfg.auto_build_entries+1))
       ||base>UINT64_MAX-extra)return PFO_RESOURCE;
    *core_work=base;*family_work=extra;return PFR_OK;
}
int pfm2_prepare(void *opaque,const void *a,void *b,const void *chunk,size_t n,
                 void *cp,size_t capacity,pfo_prepared *result)
{
    const context *c=opaque;const uint8_t *raw=chunk;side *s;pfm_core_view view;
    pfm_external ext,*policy=NULL;pfm_window window;uint64_t core_work,family_work,first=0,last=UINT64_MAX;
    uint32_t ty,len;int rc,known_owner=0,known_domain=0,known_window=0;size_t written=0;
    if(result)memset(result,0,sizeof *result);
    if(!c||c->magic!=F2_MAGIC||!a||!b||a==b||!raw||!cp||!result||capacity<c->cfg.shared.checkpoint_bytes
       ||(uintptr_t)a%8||(uintptr_t)b%8||n<20)return PFR_INVALID;
    rc=pfm2_work(c,raw,n,&core_work,&family_work);
    if(rc==PFO_RESOURCE){result->reason=PFR_STATE_BYTES;return rc;}if(rc!=PFR_OK)return rc;
    if(family_work>c->cfg.shared.prepare_work||core_work>c->cfg.shared.prepare_work-family_work){
        result->reason=PFR_STATE_BYTES;return PFO_RESOURCE;
    }
    if(pfm_core_snapshot(core(c),a,&view)!=PFR_OK||!admit(view.ppu.header,c->cfg.family_flags)||view.ppu.terminal
       ||state(c,a)->magic!=F2_SIDE)return PFR_INVALID;
    ty=r32(raw);len=r32(raw+4);
    if(r64(raw+8)!=view.ppu.next)return PFR_INVALID;
    memcpy((uint8_t *)b+c->core_layout.arena_bytes,(const uint8_t *)a+c->core_layout.arena_bytes,
        c->layout.arena_bytes-c->core_layout.arena_bytes);s=state(c,b);
    if(s->fifo.run_count>c->cfg.fifo_membership_runs||s->fifo.group_count>c->cfg.accounting_groups||s->windows>c->cfg.windows)return PFR_INVALID;
    if((ty>=31&&ty<=33)||ty==43){
        if(!(c->cfg.family_flags&64))return PFR_INVALID;
        ext.type=ty;ext.storage=ty==31?PFM_EXTERNAL_APPEND:ty==33?PFM_EXTERNAL_LATEST:PFM_EXTERNAL_EVENT;policy=&ext;
        if(ty==31&&len==64){
            known_owner=pfm_core_declared(core(c),a,r32(raw+28));
            known_domain=pfm_core_rsx_declared(core(c),a,r32(raw+24));
            if(known_owner<0||known_domain<0)return PFR_INVALID;
        }
        if(ty==43&&len==80&&(r32(view.ppu.header+12)&512)){
            if(pfm_core_window(core(c),a,r64(raw+48),&window)==PFR_OK){
                known_window=1;first=window.first_frame;last=window.closed?window.last_frame:UINT64_MAX;
            }
        }
        rc=fi_accept(&s->fifo,runs(c,b),c->cfg.fifo_membership_runs,groups(c,b),c->cfg.accounting_groups,
            ty,raw+16,len,view.ppu.next,view.ppu.clock_frequency,known_owner,known_domain,
            (r32(view.ppu.header+12)&512)!=0,known_window,first,last);
        if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}if(rc!=PFR_OK)return rc;
    }
    if(ty>=34&&ty<=38){
        if(!(c->cfg.family_flags&128))return PFR_INVALID;
        ext.type=ty;ext.storage=ty==35?PFM_EXTERNAL_EVENT:ty==37?PFM_EXTERNAL_LATEST:PFM_EXTERNAL_APPEND;policy=&ext;
        if(ty==34&&(len==160||len==176||len==216)&&r16(raw+18)==0){
            known_owner=pfm_core_declared(core(c),a,r32(raw+32));known_domain=pfm_core_rsx_declared(core(c),a,r32(raw+24));
            if(known_owner<0||known_domain<0)return PFR_INVALID;
        }
        rc=au_accept(&s->automatic,auto_pools(c,b),&c->cfg,ty,raw+16,len,view.ppu.next,
            view.ppu.clock_frequency,known_owner,known_domain);
        if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}if(rc!=PFR_OK)return rc;
    }
    rc=pfm_core_accept(core(c),a,b,raw,n,policy,c->cfg.shared.prepare_work-family_work,result);if(rc!=PFR_OK)return rc;
    if(ty==31||(ty==34&&r16(raw+18)==0)){rc=pfm_core_bind_rsx(core(c),b,r32(raw+24));if(rc!=PFR_OK)return rc;}
    if(ty==41){if(s->windows==c->cfg.windows){result->reason=PFR_IDENTITIES;return PFO_RESOURCE;}++s->windows;}
    if(ty==42){rc=fi_close(&s->fifo,r32(raw+20),r64(raw+28));
        if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}if(rc!=PFR_OK)return rc;
    }
    if(!result->terminal){rc=encode(c,b,cp,capacity,&written);
        if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}if(rc!=PFR_OK)return rc;
    }
    result->checkpoint_bytes=written;return PFR_OK;
}
