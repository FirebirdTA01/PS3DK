/* Private original-context PPU/PROFILE/SPU PFRMIX01 schema-1 encoder. */
#include "roll_combined.h"
#include "roll_combined_internal.h"
#include <string.h>
#define MAGIC UINT32_C(0x50464d43)
#define SIDE_MAGIC UINT32_C(0x50464d53)
typedef struct { uint32_t magic,count; } side;
static uint16_t r16(const uint8_t *p){return (uint16_t)((unsigned)p[0]|(unsigned)p[1]<<8);}
static uint32_t r32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t r64(const uint8_t *p){return (uint64_t)r32(p)|(uint64_t)r32(p+4)<<32;}
static void w16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void w32(uint8_t *p,uint32_t n){unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void w64(uint8_t *p,uint64_t n){unsigned i;for(i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t crc(const uint8_t *p,size_t n)
{uint32_t c=UINT32_MAX;unsigned i;while(n--){c^=*p++;for(i=0;i<8;++i)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}return ~c;}
static int add(size_t *a,size_t n){if(n>SIZE_MAX-*a)return 0;*a+=n;return 1;}
static int mul(size_t *a,uint32_t n,size_t size){if(n&&size>SIZE_MAX/n)return 0;return add(a,(size_t)n*size);}
static int fit(size_t n)
{
#if SIZE_MAX > UINT32_MAX
    return n<=UINT32_MAX;
#else
    (void)n;return 1;
#endif
}
static int align8(size_t *n){return add(n,(8-*n%8)%8)&&fit(*n);}
#include "roll_combined_spu.h"
typedef struct {
    uint32_t magic, core_context_offset, side_offset, counts_offset, spu_offset, family_flags, rsx_domains;
    pfm_config cfg;pfm_layout layout;pfp_config ppu_cfg;pfp_layout ppu_layout;sp_layout spu_layout;
} context;
_Static_assert(_Alignof(context)<=8&&_Alignof(pfp_record_chunks)<=8&&_Alignof(sp_state)<=8
    &&_Alignof(sp_domain)<=8&&_Alignof(sp_lane)<=8&&_Alignof(sp_entry)<=8
    &&_Alignof(sp_run)<=8&&_Alignof(sp_node)<=8,"combined arena alignment");
static int plan_extended(const pfm_config *cfg,uint32_t family,uint32_t rsx,context *c)
{
    size_t bound=148,n;
    if(!cfg||cfg->metadata_bytes<16||!cfg->prepare_work||(family!=0&&family!=64)||rsx>64||(!family&&rsx))return PFR_INVALID;
    memset(c,0,sizeof *c);c->cfg=*cfg;c->family_flags=family;c->rsx_domains=rsx;
    if(!add(&bound,cfg->metadata_bytes-16)||!mul(&bound,cfg->incarnations,128)||!mul(&bound,cfg->ppu_stack_rows,80)
       ||!mul(&bound,cfg->frame_identities,16)||!mul(&bound,cfg->journal_entries,32)||!mul(&bound,cfg->event_runs,64)
       ||!mul(&bound,cfg->saved_spu_witnesses,24)||!fit(bound)||bound>cfg->checkpoint_bytes)return PFR_INVALID;
    c->ppu_cfg.incarnations=cfg->incarnations;c->ppu_cfg.descriptors=cfg->descriptors;
    c->ppu_cfg.stack_rows=cfg->ppu_stack_rows;c->ppu_cfg.frame_identities=cfg->frame_identities;
    c->ppu_cfg.metadata_bytes=cfg->metadata_bytes-16;c->ppu_cfg.checkpoint_bytes=cfg->checkpoint_bytes;
    if(pfp_plan(&c->ppu_cfg,&c->ppu_layout)!=PFR_OK)return PFR_INVALID;
    n=sizeof *c;
    if(!align8(&n))return PFR_INVALID;
    c->core_context_offset=(uint32_t)n;
    if(!add(&n,c->ppu_layout.context_bytes)||!align8(&n))return PFR_INVALID;
    c->layout.context_bytes=n;
    n=c->ppu_layout.arena_bytes;
    if(!align8(&n))return PFR_INVALID;
    c->side_offset=(uint32_t)n;
    if(!add(&n,sizeof(side))||!align8(&n))return PFR_INVALID;
    c->counts_offset=(uint32_t)n;
    if(!mul(&n,cfg->incarnations,sizeof(pfp_record_chunks))||!align8(&n))return PFR_INVALID;
    c->spu_offset=(uint32_t)n;
    if(!sp_plan(cfg,rsx,family!=0,&c->spu_layout)||!add(&n,c->spu_layout.bytes)||!align8(&n))return PFR_INVALID;
    c->layout.arena_bytes=n;c->layout.arena_alignment=8;c->layout.checkpoint_bound=bound;c->magic=MAGIC;return PFR_OK;
}
static int plan(const pfm_config *cfg,context *c){return plan_extended(cfg,0,0,c);}
int pfm_plan(const pfm_config *cfg,pfm_layout *out)
{context c;int rc;if(!out)return PFR_INVALID;rc=plan(cfg,&c);if(rc==PFR_OK)*out=c.layout;return rc;}
int pfm_core_plan(const pfm_core_config *cfg,pfm_layout *out)
{context c;int rc;if(!cfg||!out)return PFR_INVALID;rc=plan_extended(&cfg->base,cfg->family_flags,cfg->rsx_domains,&c);
    if(rc==PFR_OK)*out=c.layout;
    return rc;}
static void *core(const context *c){return (uint8_t *)(void *)c+c->core_context_offset;}
static side *extra(const context *c,void *a){return (side *)((uint8_t *)a+c->side_offset);}
static pfp_record_chunks *counts(const context *c,void *a){return (pfp_record_chunks *)((uint8_t *)a+c->counts_offset);}
static sp_state *spu(const context *c,const void *a){return (sp_state *)((uint8_t *)(void *)a+c->spu_offset);}
static int admit_core(const uint8_t *h,uint32_t family)
{uint32_t flags,base;if(!h||memcmp(h,"PS3DKPRF",8)||r16(h+8)!=1)return 0;flags=r32(h+12);base=flags&7;
 return !(flags&~(UINT32_C(519)|family))&&(flags&192)==family&&(base==1||base==3||base==7);}
static int admit(const uint8_t *h){return admit_core(h,0);}
static void section(uint8_t *p,uint16_t kind,uint32_t count,uint32_t bytes)
{w16(p,kind);w16(p+2,1);w32(p+4,count);w32(p+8,bytes);w32(p+12,0);}
static int plus64(uint64_t *n,uint64_t v){if(v>UINT64_MAX-*n)return 0;*n+=v;return 1;}
static int times64(uint64_t *n,uint64_t count,uint64_t size)
{if(count&&size>UINT64_MAX/count)return 0;return plus64(n,count*size);}
int pfm_core_snapshot(const void *opaque,const void *a,pfm_core_view *out)
{
    const context *c=opaque;const side *s;const pfp_record_chunks *cs;sp_state *sp;uint64_t bytes;uint32_t i;
    if(!c||c->magic!=MAGIC||!a||!out||(uintptr_t)a%8)return PFR_INVALID;
    s=extra(c,(void *)a);cs=counts(c,(void *)a);sp=spu(c,a);
    if(s->magic!=SIDE_MAGIC||s->count>c->cfg.incarnations||pfp_core_view(core(c),a,&out->ppu)!=PFR_OK)return PFR_INVALID;
    out->ppu_record_chunks=0;
    for(i=0;i<s->count;++i)if(!plus64(&out->ppu_record_chunks,cs[i].chunks))return PFR_INVALID;
    out->spu_event_chunks=sp->chunks;out->spu_events=sp->events;out->journal_entries=sp->entries;
    bytes=48+(uint64_t)out->ppu.lanes*128+(uint64_t)out->ppu.stacks*80
        +((uint64_t)out->ppu.frames+out->ppu.tokens)*16+sp_wire_size(sp);
    if(bytes>UINT32_MAX)return PFR_INVALID;
    out->sections_bytes=(size_t)bytes;return PFR_OK;
}
int pfm_core_sections(const void *opaque,const void *a,void *output,size_t capacity,size_t *written)
{
    const context *c=opaque;pfm_core_view view;const side *s;sp_state *sp;uint8_t *out=output;size_t at=0,n=0;int rc;
    if(written)*written=0;
    if(!written||!output||pfm_core_snapshot(c,a,&view)!=PFR_OK)return PFR_INVALID;
    if(view.sections_bytes>capacity||view.sections_bytes>c->cfg.checkpoint_bytes)return PFR_CHECKPOINT_BYTES;
    s=extra(c,(void *)a);sp=spu(c,a);
    rc=pfp_core_sections(core(c),a,counts(c,(void *)a),s->count,out,capacity,&at);if(rc!=PFR_OK)return rc;
    section(out+at,4,sp->entries,(uint32_t)sp_wire_size(sp));at+=16;
    rc=sp_encode(sp,&c->spu_layout,&view.ppu,out+at,&n);if(rc!=PFR_OK)return rc;
    if(n!=view.sections_bytes-at)return PFR_INVALID;
    *written=at+n;return PFR_OK;
}
static int encode(const context *c,const void *a,void *cp,size_t capacity,size_t *written)
{
    pfm_core_view view;pfp_view v;uint8_t *b=cp;uint64_t length,expected_bytes,expected_chunks;
    size_t at,wrote=0;uint32_t i;int rc;
    if(c->family_flags||pfm_core_snapshot(c,a,&view)!=PFR_OK)return PFR_INVALID;
    v=view.ppu;length=100+(uint64_t)v.metadata_bytes+view.sections_bytes;
    if(length>capacity||length>c->cfg.checkpoint_bytes)return PFR_CHECKPOINT_BYTES;
    expected_chunks=v.metadata_count;
    if(!plus64(&expected_chunks,view.ppu_record_chunks)||!plus64(&expected_chunks,view.spu_event_chunks)||expected_chunks!=v.next)return PFR_INVALID;
    if((uint64_t)v.metadata_count*4>v.metadata_bytes)return PFR_INVALID;
    expected_bytes=v.metadata_bytes-(uint64_t)v.metadata_count*4;
    if(!times64(&expected_bytes,view.ppu_record_chunks,28)||!times64(&expected_bytes,v.physical_rows,32)
       ||!times64(&expected_bytes,view.spu_event_chunks,40)||!times64(&expected_bytes,view.spu_events,8))return PFR_COUNTER_RANGE;
    if(expected_bytes!=v.bytes)return PFR_INVALID;
    memset(b,0,80);memcpy(b,"PFRMIX01",8);w16(b+8,1);w16(b+10,80);w32(b+12,4);memcpy(b+16,v.header,16);
    w64(b+32,v.next);w64(b+40,v.bytes);w64(b+48,length-84);w32(b+56,3);
    for(i=0;i<4;++i)w16(b+64+2*i,1);
    section(b+80,1,v.metadata_count,v.metadata_bytes);memcpy(b+96,v.metadata,v.metadata_bytes);at=96+v.metadata_bytes;
    rc=pfm_core_sections(c,a,b+at,capacity-at,&wrote);if(rc!=PFR_OK)return rc;at+=wrote;
    if(at!=length-4)return PFR_INVALID;
    w32(b+at,crc(b,at));*written=at+4;return PFR_OK;
}
int pfm_core_init(void *opaque,size_t context_bytes,const pfm_core_config *cfg,const uint8_t h[16],void *a,size_t arena_bytes)
{
    context c,*out=opaque;int rc;
    if(!cfg||!opaque||!a||!admit_core(h,cfg->family_flags)||plan_extended(&cfg->base,cfg->family_flags,cfg->rsx_domains,&c)!=PFR_OK
       ||context_bytes<c.layout.context_bytes||arena_bytes<c.layout.arena_bytes
       ||(uintptr_t)opaque%8||(uintptr_t)a%8)return PFR_INVALID;
    memset(out,0,c.layout.context_bytes);memcpy(out,&c,sizeof c);memset(a,0,c.layout.arena_bytes);
    rc=pfp_core_init(core(out),c.ppu_layout.context_bytes,&c.ppu_cfg,h,a,c.ppu_layout.arena_bytes);if(rc!=PFR_OK)return rc;
    extra(out,a)->magic=SIDE_MAGIC;return PFR_OK;
}
int pfm_init(void *opaque,size_t context_bytes,const pfm_config *cfg,const uint8_t h[16],
             void *a,size_t arena_bytes,void *cp,size_t capacity,size_t *written)
{
    pfm_core_config shared;int rc;
    if(written)*written=0;
    if(!cfg||!cp||!written||!admit(h)||capacity<cfg->checkpoint_bytes)return PFR_INVALID;
    memset(&shared,0,sizeof shared);shared.base=*cfg;
    rc=pfm_core_init(opaque,context_bytes,&shared,h,a,arena_bytes);if(rc!=PFR_OK)return rc;
    rc=encode(opaque,a,cp,capacity,written);return rc==PFR_OK?PFR_OK:PFR_INVALID;
}
/* PROFILE structural rules, profile.rs::parse; no table qualification. */
static int profile(const uint8_t *p,size_t n,uint64_t clock)
{
    static const uint8_t widths[7]={4,4,8,4,1,1,40};
    static const uint8_t masks[7]={26,10,2,26,4,4,2};
    size_t at=0,j;uint32_t bits,kinds;unsigned len,i;
    if(n<5||r16(p)!=1||r16(p+2))return 0;
    len=p[4];at=5;if(!len||len>32||len>n-at)return 0;
    for(i=0;i<len;++i){unsigned ch=p[at+i];if((!i&&(ch<'A'||ch>'Z'))
        ||!((ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='-'))return 0;}
    at+=len;if(n-at<46||r32(p+at)!=1)return 0;at+=36;
    bits=r32(p+at);kinds=r32(p+at+4);at+=8;if((bits&~UINT32_C(2047))||!kinds||(kinds&~3u)||r16(p+at)!=7)return 0;at+=2;
    for(i=0;i<7;++i){unsigned status,width;if(n-at<4||r16(p+at)!=i+1)return 0;
        status=p[at+2];width=p[at+3];at+=4;
        if(status<1||status>4||!(masks[i]&(1u<<status))||width!=widths[i]||width>n-at)return 0;
        if(status==3||status==4)for(j=0;j<width;++j)if(p[at+j])return 0;
        if(i==2&&r64(p+at)!=clock)return 0;
        at+=width;
    }
    return at==n;
}
/* Conservative structural-work ceiling: copies/writes plus bounded scans,
 * stack serialization (depth<=256), and token/frame membership scans. */
static int work(const context *c,size_t n,uint32_t ty,uint32_t owner_rows,uint64_t *quote)
{
    uint64_t total=32,per=32;
    if(!plus64(&total,c->layout.arena_bytes)||!times64(&total,c->layout.checkpoint_bound,4)
       ||!times64(&per,c->cfg.incarnations,8)||!plus64(&per,c->cfg.descriptors)
       ||!times64(&per,c->cfg.frame_identities,2)||!plus64(&per,c->cfg.ppu_stack_rows)
       ||!times64(&total,n,per)||!times64(&total,c->cfg.ppu_stack_rows,256)
       ||!times64(&total,c->cfg.incarnations,c->cfg.incarnations)
       ||!times64(&total,c->cfg.frame_identities,(uint64_t)c->cfg.incarnations+c->cfg.frame_identities)
       ||!times64(&total,c->cfg.saved_spu_witnesses,128))return 0;
    if((ty>=8&&ty<=13)||ty==15||ty==16||ty==18){
        uint64_t per_spu=32;
        if(!plus64(&per_spu,c->cfg.spu_domains)||!plus64(&per_spu,c->rsx_domains)||!plus64(&per_spu,c->cfg.spu_lanes)
           ||!plus64(&per_spu,c->cfg.spu_images)||!plus64(&per_spu,c->cfg.image_descriptors)
           ||!plus64(&per_spu,c->cfg.live_spu_stack_rows)||!plus64(&per_spu,c->cfg.event_runs)
           ||!times64(&total,n,per_spu))return 0;
    }
    if(ty==12&&!times64(&total,owner_rows,c->cfg.metadata_bytes))return 0;
    /* Four bounded common-output passes reserve section writes, support-index
     * resolution, snapshots and final envelope CRC. Fixed CRC bit operations
     * are a constant factor of these logical byte/row work units. */
    *quote=total;return 1;
}
int pfm_core_work(const void *opaque,const void *chunk,size_t n,uint64_t *quote)
{
    const context *c=opaque;const uint8_t *raw=chunk;uint32_t ty,len;
    if(quote)*quote=0;
    if(!c||c->magic!=MAGIC||!raw||!quote||n<20||n>UINT32_C(1048576)+20)return PFR_INVALID;
    ty=r32(raw);len=r32(raw+4);
    if((uint64_t)len+20!=n||(ty==12&&(len<4||r32(raw+16)>64)))return PFR_INVALID;
    return work(c,n,ty,ty==12?r32(raw+16):0,quote)?PFR_OK:PFO_RESOURCE;
}
int pfm_core_accept(void *opaque,const void *a,void *b,const void *chunk,size_t n,
    const pfm_external *external,uint64_t allowance,pfo_prepared *result)
{
    const context *c=opaque;const uint8_t *raw=chunk;pfp_view v;side *s;pfp_record_chunks *cs;
    uint32_t ty,len,flags,i;uint64_t quote;int rc;
    if(result)memset(result,0,sizeof *result);
    if(!c||c->magic!=MAGIC||!a||!b||a==b||!raw||!result
       ||(uintptr_t)a%8||(uintptr_t)b%8||n<20||n>UINT32_C(1048576)+20)return PFR_INVALID;
    rc=pfm_core_work(c,raw,n,&quote);
    if(rc==PFO_RESOURCE){result->reason=PFR_STATE_BYTES;return rc;}
    if(rc!=PFR_OK)return rc;
    if(quote>allowance||quote>c->cfg.prepare_work){result->reason=PFR_STATE_BYTES;return PFO_RESOURCE;}
    ty=r32(raw);len=r32(raw+4);
    if(crc(raw,n-4)!=r32(raw+n-4))return PFR_INVALID;
    if(pfp_core_view(core(c),a,&v)!=PFR_OK||!admit_core(v.header,c->family_flags)||v.terminal
       ||extra(c,(void *)a)->magic!=SIDE_MAGIC||extra(c,(void *)a)->count>c->cfg.incarnations)return PFR_INVALID;
    flags=r32(v.header+12);
    if(external){
        if(c->family_flags!=64||external->type!=ty
           ||!(((ty==32||ty==43)&&external->storage==PFM_EXTERNAL_EVENT)
                ||(ty==31&&external->storage==PFM_EXTERNAL_APPEND)||(ty==33&&external->storage==PFM_EXTERNAL_LATEST)))return PFR_INVALID;
    }
    if(r64(raw+8)!=v.next||!((ty>=1&&ty<=13)||(ty>=15&&ty<=18)||((flags&512)&&(ty==41||ty==42))||external)
       ||((flags&2)&&v.next==2&&ty!=17)||(ty==17&&(!(flags&2)||v.next!=2)))return PFR_INVALID;
    if(ty==17&&!profile(raw+16,len,v.clock_frequency))return PFR_INVALID;
    /* Only the sidecar is copied here. The shared accept validates framing and
     * clones its PPU arena itself; no state is reconstructed from checkpoints. */
    memcpy((uint8_t *)b+c->side_offset,(const uint8_t *)a+c->side_offset,c->layout.arena_bytes-c->side_offset);
    if((ty>=8&&ty<=13)||ty==15||ty==16||ty==18){
        rc=sp_accept(spu(c,b),&c->spu_layout,&c->cfg,&v,ty,raw+16,len,flags);
        if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}
        if(rc!=PFR_OK)return rc;
    }
    if(ty==6&&sp_terminal(spu(c,b),&c->spu_layout)!=PFR_OK)return PFR_INVALID;
    if(external)rc=pfp_core_accept_external(core(c),a,b,chunk,n,external->type,external->storage,result);
    else rc=pfp_core_accept(core(c),a,b,chunk,n,ty>=8&&ty<=18?ty:0,result);
    if(rc!=PFR_OK)return rc;
    s=extra(c,b);cs=counts(c,b);
    if(ty==5){uint32_t inc=r32(raw+16);for(i=0;i<s->count&&cs[i].incarnation!=inc;++i){}
        if(i==s->count){
            if(s->count==c->cfg.incarnations){result->reason=PFR_IDENTITIES;return PFO_RESOURCE;}
            cs[i].incarnation=inc;cs[i].reserved=0;cs[i].chunks=0;++s->count;
        }
        if(cs[i].chunks==UINT64_MAX){result->reason=PFR_COUNTER_RANGE;return PFO_RESOURCE;}++cs[i].chunks;
    }
    return PFR_OK;
}
int pfm_prepare(void *opaque,const void *a,void *b,const void *chunk,size_t n,void *cp,size_t capacity,pfo_prepared *result)
{
    const context *c=opaque;size_t written=0;int rc;
    if(result)memset(result,0,sizeof *result);
    if(!c||c->magic!=MAGIC||c->family_flags||!cp||!result||capacity<c->cfg.checkpoint_bytes)return PFR_INVALID;
    rc=pfm_core_accept(opaque,a,b,chunk,n,NULL,c->cfg.prepare_work,result);if(rc!=PFR_OK)return rc;
    if(!result->terminal){rc=encode(c,b,cp,capacity,&written);
        if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}if(rc!=PFR_OK)return rc;
    }
    result->checkpoint_bytes=written;return PFR_OK;
}
int pfm_core_declared(const void *opaque,const void *a,uint32_t incarnation)
{
    pfm_core_view v;if(pfm_core_snapshot(opaque,a,&v)!=PFR_OK)return PFR_INVALID;
    return incarnation?sp_owner(&v.ppu,incarnation):0;
}
int pfm_core_rsx_declared(const void *opaque,const void *a,uint32_t id)
{
    const context *c=opaque;pfm_core_view v;sp_domain *d;
    if(pfm_core_snapshot(c,a,&v)!=PFR_OK)return PFR_INVALID;
    d=sp_domain_find(spu(c,a),&c->spu_layout,id);return d&&d->kind==2;
}
int pfm_core_bind_rsx(const void *opaque,void *a,uint32_t id)
{
    const context *c=opaque;sp_domain *d;if(pfm_core_rsx_declared(c,a,id)!=1)return PFR_INVALID;
    d=sp_domain_find(spu(c,a),&c->spu_layout,id);d->bound=1;return PFR_OK;
}
int pfm_core_window(const void *opaque,const void *a,uint64_t id,pfm_window *out)
{
    pfm_core_view v;size_t at=0;uint32_t ordinal;int found=0;pfm_window window;
    if(!out||!id||id>UINT32_MAX||pfm_core_snapshot(opaque,a,&v)!=PFR_OK)return PFR_INVALID;
    memset(&window,0,sizeof window);
    for(ordinal=0;ordinal<v.ppu.metadata_count;++ordinal){uint32_t n,ty;const uint8_t *raw;
        if(at>v.ppu.metadata_bytes||v.ppu.metadata_bytes-at<4)return PFR_INVALID;
        n=r32(v.ppu.metadata+at);at+=4;if(n<20||n>v.ppu.metadata_bytes-at)return PFR_INVALID;
        raw=v.ppu.metadata+at;at+=n;ty=r32(raw);
        if(ty!=41&&ty!=42)continue;
        if(n!=52)return PFR_INVALID;
        if(r32(raw+20)!=id)continue;
        if(ty==41){if(found)return PFR_INVALID;found=1;window.first_frame=r64(raw+28);}
        else{if(!found||window.closed)return PFR_INVALID;window.closed=1;window.last_frame=r64(raw+28);}
    }
    if(!found)return PFR_INVALID;
    *out=window;return PFR_OK;
}
