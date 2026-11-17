/* Private PPU/WINDOWS accepted-state encoder. No runtime installation, IO,
 * allocation, extension-family acceptance or historical event replay. */
#include "roll_ppu.h"
#include "roll_ppu_internal.h"
#include <string.h>

#define CONTEXT_MAGIC UINT32_C(0x50465043)
#define ARENA_MAGIC UINT32_C(0x50465041)
#define NONE UINT32_MAX
#define SEQ_LIMIT (UINT64_C(1)<<63)
#define MAX_CHUNK UINT32_C(1048576)
typedef struct {
    uint32_t magic,arena_bytes;
    uint8_t header[16];
    uint64_t next,bytes,total_written;
    uint32_t metadata_used,metadata_count,lane_count,descriptor_count;
    uint32_t stacks_used,frames_used,tokens_used,free_stack,free_frame;
    uint32_t have_header,have_clock,terminal,max_threads,max_descriptors;
    uint32_t build_offset,build_length;
    uint32_t window_open,window_last_id,window_id,window_kind,window_count,window_previous;
    uint64_t window_first,window_last_frame;
} state;
typedef struct {
    uint32_t inc,slot,flags,depth,stack,frame_head,frame_tail,frame_runs;
    uint64_t written,gaps,gap_count,regerr,next,max_span,last_tb,next_frame;
    uint64_t frame,last_frame_seq,last_frame_tb;
    uint32_t final_seen,final_state,final_limits,reserved;
    uint64_t final_dropped;
} lane;
typedef struct { uint32_t index,scope; uint64_t id; } descriptor;
typedef struct {
    uint32_t next,used;
    uint8_t begin[32],parent[32];
    uint32_t context,affected;
    uint64_t frame;
} stack_node;
typedef struct { uint32_t next,used; uint64_t first,last; } frame_node;
typedef struct { uint32_t owner,reserved; uint64_t frame; } token;
typedef struct { uint32_t magic,reserved; pfp_config cfg; pfp_layout layout; } context;
typedef struct { const uint8_t *p; size_t n,at; int bad; } cursor;
typedef struct { const uint8_t *p; size_t n,at; } string_view;
typedef struct { uint8_t *p; size_t n,cap; int bad; } writer;
_Static_assert(_Alignof(state)<=8 && _Alignof(lane)<=8 && _Alignof(descriptor)<=8,"fixed arena alignment");
_Static_assert(_Alignof(stack_node)<=8 && _Alignof(frame_node)<=8 && _Alignof(token)<=8,"fixed pool alignment");
static uint16_t r16(const uint8_t *p){return (uint16_t)((unsigned)p[0]|(unsigned)p[1]<<8);}
static uint32_t r32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint64_t r64(const uint8_t *p){return (uint64_t)r32(p)|(uint64_t)r32(p+4)<<32;}
static void w32(uint8_t *p,uint32_t n){unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;unsigned i;while(n--){c^=*p++;for(i=0;i<8;++i)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}return ~c;}
static int add(size_t *n,size_t v){if(v>SIZE_MAX-*n)return 0;*n+=v;return 1;}
static int mul_add(size_t *n,size_t count,size_t size)
{if(count&&size>SIZE_MAX/count)return 0;return add(n,count*size);}
static int offset_fits(size_t n)
{
#if SIZE_MAX > UINT32_MAX
    return n<=UINT32_MAX;
#else
    (void)n;return 1;
#endif
}
static int partition(size_t *n,uint32_t count,size_t size,uint32_t *off)
{
    if(!add(n,(8-*n%8)%8)||!offset_fits(*n))return 0;
    *off=(uint32_t)*n;return mul_add(n,count,size)&&offset_fits(*n);
}
int pfp_plan(const pfp_config *cfg,pfp_layout *layout)
{
    pfp_layout p;size_t n=sizeof(state),bound=132;
    if(!cfg||!layout||cfg->checkpoint_bytes<132)return PFR_INVALID;
    memset(&p,0,sizeof p);
    if(!partition(&n,cfg->incarnations,sizeof(lane),&p.lanes_offset)
       ||!partition(&n,cfg->descriptors,sizeof(descriptor),&p.descriptors_offset)
       ||!partition(&n,cfg->stack_rows,sizeof(stack_node),&p.stacks_offset)
       ||!partition(&n,cfg->frame_identities,sizeof(frame_node),&p.frames_offset)
       ||!partition(&n,cfg->frame_identities,sizeof(token),&p.tokens_offset)
       ||!partition(&n,cfg->metadata_bytes,1,&p.metadata_offset)
       ||!add(&n,(8-n%8)%8)||!offset_fits(n))return PFR_INVALID;
    if(!add(&bound,cfg->metadata_bytes)||!mul_add(&bound,cfg->incarnations,120)
       ||!mul_add(&bound,cfg->stack_rows,80)||!mul_add(&bound,cfg->frame_identities,16))return PFR_INVALID;
    p.context_bytes=sizeof(context);p.arena_bytes=n;p.arena_alignment=8;p.checkpoint_bound=bound;
    *layout=p;return PFR_OK;
}
static const uint8_t *take(cursor *c,size_t n)
{const uint8_t *p;if(c->bad||n>c->n-c->at){c->bad=1;return NULL;}p=c->p+c->at;c->at+=n;return p;}
static uint8_t c8(cursor *c){const uint8_t *p=take(c,1);return p?*p:0;}
static uint16_t c16(cursor *c){const uint8_t *p=take(c,2);return p?r16(p):0;}
static uint32_t c32(cursor *c){const uint8_t *p=take(c,4);return p?r32(p):0;}
static uint64_t c64(cursor *c){const uint8_t *p=take(c,8);return p?r64(p):0;}
static int utf8(const uint8_t *p,size_t n)
{
    size_t i=0;unsigned k,need,low,high,x;
    while(i<n){x=p[i++];if(x==0)return 0;if(x<128)continue;
        low=0x80;high=0xbf;
        if(x>=0xc2&&x<=0xdf)need=1;
        else if(x>=0xe0&&x<=0xef){need=2;if(x==0xe0)low=0xa0;if(x==0xed)high=0x9f;}
        else if(x>=0xf0&&x<=0xf4){need=3;if(x==0xf0)low=0x90;if(x==0xf4)high=0x8f;}
        else return 0;
        if(need>n-i||p[i]<low||p[i]>high)return 0;
        for(k=1;k<need;++k)if(p[i+k]<0x80||p[i+k]>0xbf)return 0;
        i+=need;
    }return 1;
}
static string_view str(cursor *c,int raw)
{
    string_view s;s.n=c16(c);s.at=c->at;s.p=NULL;
    if(c->bad||s.n>512){c->bad=1;return s;}
    s.p=take(c,s.n);if(!s.p||(!raw&&!utf8(s.p,s.n)))c->bad=1;return s;
}
static int done(const cursor *c){return !c->bad&&c->at==c->n;}
static lane *lanes(const context *c,void *a){return (lane *)((uint8_t *)a+c->layout.lanes_offset);}
static descriptor *descriptors(const context *c,void *a){return (descriptor *)((uint8_t *)a+c->layout.descriptors_offset);}
static stack_node *stacks(const context *c,void *a){return (stack_node *)((uint8_t *)a+c->layout.stacks_offset);}
static frame_node *frames(const context *c,void *a){return (frame_node *)((uint8_t *)a+c->layout.frames_offset);}
static token *tokens(const context *c,void *a){return (token *)((uint8_t *)a+c->layout.tokens_offset);}
static lane *find_lane(const context *c,state *s,uint32_t inc)
{lane *l=lanes(c,s);uint32_t i;for(i=0;i<s->lane_count;++i)if(l[i].inc==inc)return &l[i];return NULL;}
static int have_desc(const context *c,state *s,uint32_t index)
{descriptor *d=descriptors(c,s);uint32_t i;for(i=0;i<s->descriptor_count;++i)if(d[i].index==index)return 1;return 0;}
static int frame_member(const context *c,state *s,const lane *l,uint64_t frame)
{
    frame_node *fs=frames(c,s);uint32_t index=l->frame_head;
    while(index!=NONE){if(frame<fs[index].first)return 0;if(frame<=fs[index].last)return 1;index=fs[index].next;}
    return 0;
}
static int remember_token(const context *c,state *s,uint32_t owner,uint64_t frame)
{
    lane *l=find_lane(c,s,owner);token *ts=tokens(c,s);uint32_t at=0;
    if(!l)return PFR_INVALID;
    if(frame_member(c,s,l,frame))return PFR_OK;
    while(at<s->tokens_used&&(ts[at].owner<owner||(ts[at].owner==owner&&ts[at].frame<frame)))++at;
    if(at<s->tokens_used&&ts[at].owner==owner&&ts[at].frame==frame)return PFR_OK;
    /* Individual transient pool bounds are hard. The shared run+token bound
     * is checked only after the complete candidate has resolved its frames. */
    if(s->tokens_used==c->cfg.frame_identities)return PFR_IDENTITIES;
    memmove(&ts[at+1],&ts[at],(s->tokens_used-at)*sizeof *ts);
    ts[at].owner=owner;ts[at].reserved=0;ts[at].frame=frame;++s->tokens_used;return PFR_OK;
}
static void resolve_token(const context *c,state *s,uint32_t owner,uint64_t frame)
{
    token *ts=tokens(c,s);uint32_t i;
    for(i=0;i<s->tokens_used;++i)if(ts[i].owner==owner&&ts[i].frame==frame){
        --s->tokens_used;memmove(&ts[i],&ts[i+1],(s->tokens_used-i)*sizeof *ts);
        memset(&ts[s->tokens_used],0,sizeof *ts);return;
    }
}
static void pop_stack(const context *c,state *s,lane *l)
{
    stack_node *ns=stacks(c,s);uint32_t node=l->stack;
    l->stack=ns[node].next;--l->depth;--s->stacks_used;
    memset(&ns[node],0,sizeof ns[node]);ns[node].next=s->free_stack;s->free_stack=node;
}
static void out(writer *w,const void *p,size_t n)
{if(w->bad||n>w->cap-w->n){w->bad=1;return;}memcpy(w->p+w->n,p,n);w->n+=n;}
static void o16(writer *w,uint16_t n){uint8_t p[2]={(uint8_t)n,(uint8_t)(n>>8)};out(w,p,2);}
static void o32(writer *w,uint32_t n){uint8_t p[4];w32(p,n);out(w,p,4);}
static void o64(writer *w,uint64_t n){uint8_t p[8];unsigned i;for(i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));out(w,p,8);}
static void section(writer *w,uint16_t kind,uint32_t count,uint32_t bytes)
{o16(w,kind);o16(w,1);o32(w,count);o32(w,bytes);o32(w,0);}
int pfp_core_sections(const void *opaque,const void *arena,const pfp_record_chunks *counts,uint32_t count,
    void *buffer,size_t capacity,size_t *written)
{
    const context *c=opaque;state *s=(state *)arena;
    lane *ls=lanes(c,s);stack_node *ns=stacks(c,s);frame_node *fs=frames(c,s);token *ts=tokens(c,s);
    uint32_t fixed=counts?128:120;
    uint64_t row_bytes=(uint64_t)s->lane_count*fixed+(uint64_t)s->stacks_used*80+(uint64_t)s->frames_used*16;
    uint64_t length=32+row_bytes+(uint64_t)s->tokens_used*16;
    writer w={(uint8_t *)buffer,0,capacity,0};uint32_t i,k,j,index;
    if(length>capacity||row_bytes>UINT32_MAX)return PFR_CHECKPOINT_BYTES;
    section(&w,2,s->lane_count,(uint32_t)row_bytes);
    for(i=0;i<s->lane_count;++i){lane *l=&ls[i];
        o32(&w,fixed+80*l->depth+16*l->frame_runs);o32(&w,l->inc);
        o64(&w,l->written);o64(&w,l->gaps);o64(&w,l->gap_count);o64(&w,l->regerr);o64(&w,l->next);
        o64(&w,l->max_span);o64(&w,l->last_tb);o64(&w,l->next_frame);o64(&w,l->frame);o64(&w,l->frame);
        o64(&w,l->last_frame_seq);o64(&w,l->last_frame_tb);o32(&w,l->flags);o32(&w,l->depth);o32(&w,l->frame_runs);o32(&w,0);
        if(counts){uint64_t chunks=0;for(j=0;j<count;++j)if(counts[j].incarnation==l->inc){chunks=counts[j].chunks;break;}o64(&w,chunks);}
        /* Stack pool links run top-to-bottom; wire witnesses are bottom-first.
         * Depth <=256 bounds this walk without a per-call scratch allocation. */
        for(k=l->depth;k>0;--k){index=l->stack;for(j=1;j<k;++j)index=ns[index].next;
            out(&w,ns[index].begin,32);out(&w,ns[index].parent,32);o32(&w,ns[index].context);o32(&w,ns[index].affected);o64(&w,ns[index].frame);
        }
        index=l->frame_head;for(k=0;k<l->frame_runs;++k){o64(&w,fs[index].first);o64(&w,fs[index].last);index=fs[index].next;}
    }
    section(&w,3,s->tokens_used,(uint32_t)((uint64_t)s->tokens_used*16));
    for(i=0;i<s->tokens_used;++i){o32(&w,ts[i].owner);o32(&w,0);o64(&w,ts[i].frame);}
    if(w.bad||w.n!=length)return PFR_INVALID;
    *written=w.n;return PFR_OK;
}
static int encode(const context *c,state *s,void *checkpoint,size_t capacity,size_t *written)
{
    uint64_t row_bytes=(uint64_t)s->lane_count*120+(uint64_t)s->stacks_used*80+(uint64_t)s->frames_used*16;
    uint64_t length=132+(uint64_t)s->metadata_used+row_bytes+(uint64_t)s->tokens_used*16;
    writer w={(uint8_t *)checkpoint,0,capacity,0};size_t sections=0;int rc;
    if(length>c->cfg.checkpoint_bytes||length>capacity||row_bytes>UINT32_MAX)return PFR_CHECKPOINT_BYTES;
    out(&w,"PFRCKP01",8);o16(&w,1);o16(&w,3);o32(&w,0);o64(&w,s->next);
    o32(&w,r32(s->header+12));o32(&w,0);o64(&w,s->bytes);o64(&w,length-68);o64(&w,s->total_written);o64(&w,0);
    section(&w,1,s->metadata_count,16+s->metadata_used);out(&w,s->header,16);
    out(&w,(uint8_t *)s+c->layout.metadata_offset,s->metadata_used);
    rc=pfp_core_sections(c,s,NULL,0,w.p+w.n,w.cap-w.n,&sections);if(rc!=PFR_OK)return rc;w.n+=sections;
    if(w.bad||w.n!=length-4)return PFR_INVALID;
    o32(&w,crc(w.p,w.n));if(w.bad)return PFR_INVALID;*written=w.n;return PFR_OK;
}
int pfp_core_init(void *opaque,size_t context_bytes,const pfp_config *cfg,const uint8_t header[16],void *arena,size_t arena_bytes)
{
    pfp_layout p;context *c=opaque;state *s=arena;uint32_t i;
    if(!opaque||!header||!arena||pfp_plan(cfg,&p)!=PFR_OK
       ||context_bytes<p.context_bytes||arena_bytes<p.arena_bytes
       ||(uintptr_t)opaque%_Alignof(context)||(uintptr_t)arena%p.arena_alignment
       ||memcmp(header,"PS3DKPRF",8)||r16(header+8)!=1)return PFR_INVALID;
    memset(c,0,sizeof *c);c->magic=CONTEXT_MAGIC;c->cfg=*cfg;c->layout=p;
    memset(s,0,p.arena_bytes);s->magic=ARENA_MAGIC;s->arena_bytes=(uint32_t)p.arena_bytes;memcpy(s->header,header,16);
    s->free_stack=cfg->stack_rows?0:NONE;s->free_frame=cfg->frame_identities?0:NONE;
    for(i=0;i<cfg->stack_rows;++i)stacks(c,s)[i].next=i+1<cfg->stack_rows?i+1:NONE;
    for(i=0;i<cfg->frame_identities;++i)frames(c,s)[i].next=i+1<cfg->frame_identities?i+1:NONE;
    return PFR_OK;
}
int pfp_init(void *opaque,size_t context_bytes,const pfp_config *cfg,const uint8_t header[16],
    void *arena,size_t arena_bytes,void *checkpoint,size_t capacity,size_t *written)
{
    size_t n=0;int rc;if(written)*written=0;
    if(!header||!cfg||!checkpoint||!written||capacity<cfg->checkpoint_bytes||(r32(header+12)&~UINT32_C(512)))return PFR_INVALID;
    rc=pfp_core_init(opaque,context_bytes,cfg,header,arena,arena_bytes);if(rc!=PFR_OK)return rc;
    rc=encode(opaque,arena,checkpoint,capacity,&n);if(rc!=PFR_OK)return PFR_INVALID;*written=n;return PFR_OK;
}
static int header_chunk(const context *c,state *s,cursor *p)
{
    string_view build;unsigned abi,mode;uint16_t reserved;uint32_t threads,descs;
    (void)str(p,0);build=str(p,1);(void)str(p,0);(void)str(p,0);(void)str(p,0);(void)str(p,0);
    abi=c8(p);mode=c8(p);reserved=c16(p);(void)c64(p);(void)c32(p);threads=c32(p);descs=c32(p);
    if(!done(p)||(abi!=1&&abi!=2)||(mode!=1&&mode!=2)||reserved||threads>1024||descs>65536)return PFR_INVALID;
    s->max_threads=threads;s->max_descriptors=descs;s->have_header=1;
    /* Metadata was appended before semantic admission, in inactive storage. */
    s->build_offset=c->layout.metadata_offset+s->metadata_used-(uint32_t)(p->n+20)+16+(uint32_t)build.at;
    s->build_length=(uint32_t)build.n;return PFR_OK;
}
static int clocks_chunk(state *s,cursor *p)
{
    uint32_t count=c32(p),domain=c32(p),reserved=c32(p);uint64_t freq=c64(p);(void)c64(p);
    if(!done(p)||count!=1||domain!=1||reserved||!freq)return PFR_INVALID;
    s->have_clock=1;return PFR_OK;
}
static uint64_t hash_bytes(uint64_t h,const uint8_t *p,size_t n)
{while(n--)h=(h^*p++)*UINT64_C(0x100000001b3);return h;}
static int descriptors_chunk(const context *c,state *s,cursor *p)
{
    uint32_t count=c32(p),i,k;descriptor *ds=descriptors(c,s);uint8_t zero=0;
    if(p->bad||!count||count>(p->n-p->at)/24)return PFR_INVALID;
    for(i=0;i<count;++i){descriptor d;uint32_t line;unsigned reserved;string_view name,file;uint64_t hash;uint8_t le[4];
        memset(&d,0,sizeof d);d.index=c32(p);d.id=c64(p);d.scope=c8(p);reserved=c8(p);line=c32(p);
        name=str(p,0);(void)str(p,0);file=str(p,0);
        if(p->bad||!name.n||d.index>=s->max_descriptors||(d.scope!=1&&d.scope!=2)||reserved)return PFR_INVALID;
        hash=hash_bytes(UINT64_C(0xcbf29ce484222325),(uint8_t *)s+s->build_offset,s->build_length);
        hash=hash_bytes(hash,&zero,1);hash=hash_bytes(hash,file.p,file.n);hash=hash_bytes(hash,&zero,1);
        w32(le,line);hash=hash_bytes(hash,le,4);hash=hash_bytes(hash,name.p,name.n);if(hash!=d.id)return PFR_INVALID;
        for(k=0;k<s->descriptor_count;++k)if(ds[k].index==d.index||ds[k].id==d.id)return PFR_INVALID;
        if(s->descriptor_count==c->cfg.descriptors)return PFR_IDENTITIES;
        for(k=0;k<s->descriptor_count&&ds[k].index<d.index;++k){}
        memmove(&ds[k+1],&ds[k],(s->descriptor_count-k)*sizeof *ds);ds[k]=d;++s->descriptor_count;
    }return done(p)?PFR_OK:PFR_INVALID;
}
static int threads_chunk(const context *c,state *s,cursor *p)
{
    uint32_t count=c32(p),i,k;lane *ls=lanes(c,s);
    if(p->bad||!count||count>(p->n-p->at)/18)return PFR_INVALID;
    for(i=0;i<count;++i){uint32_t inc=c32(p),slot=c32(p);lane l;(void)c64(p);(void)str(p,0);
        if(p->bad||!inc||slot>=s->max_threads)return PFR_INVALID;
        for(k=0;k<s->lane_count;++k)if(ls[k].inc==inc||(!(ls[k].flags&128)&&ls[k].slot==slot))return PFR_INVALID;
        if(s->lane_count==c->cfg.incarnations)return PFR_IDENTITIES;
        memset(&l,0,sizeof l);l.inc=inc;l.slot=slot;l.stack=l.frame_head=l.frame_tail=NONE;
        for(k=0;k<s->lane_count&&ls[k].inc<inc;++k){}
        memmove(&ls[k+1],&ls[k],(s->lane_count-k)*sizeof *ls);ls[k]=l;++s->lane_count;
    }return done(p)?PFR_OK:PFR_INVALID;
}
static int records_chunk(const context *c,state *s,cursor *p)
{
    uint32_t inc=c32(p),count=c32(p),i;lane *l=find_lane(c,s,inc);stack_node *ns=stacks(c,s);frame_node *fs=frames(c,s);int pending_gap=0;
    if(p->bad||!l||(l->flags&128)||!count||(uint64_t)count*32!=p->n-p->at)return PFR_INVALID;
    for(i=0;i<count;++i){const uint8_t *r=take(p,32);unsigned kind=r[0],depth=r[1],flags=r16(r+2);
        uint32_t desc=r32(r+4);uint64_t seq=r64(r+8),tb=r64(r+16),id=r64(r+24);
        if(seq!=l->next||seq>=SEQ_LIMIT||kind<1||kind>6)return PFR_INVALID;
        if(kind==5){uint64_t gap;
            if(flags||depth||desc||pending_gap||seq!=tb||tb>id||id>=SEQ_LIMIT||i+1==count)return PFR_INVALID;
            gap=id-tb+1;if(gap>SEQ_LIMIT-l->gaps)return PFR_INVALID;
            l->gaps+=gap;++l->gap_count;l->next=id+1;pending_gap=1;
            while(l->depth)pop_stack(c,s,l);
            l->flags|=16|8;continue;
        }
        pending_gap=0;
        if(kind==1){uint32_t node,owner;const uint8_t *parent;uint64_t parent_id,token_frame;unsigned parent_flags;int rc;
            if(flags||!id||id>=SEQ_LIMIT||id<=l->max_span||!have_desc(c,s,desc)||i+1>=count)return PFR_INVALID;
            parent=take(p,32);parent_id=r64(parent+16);parent_flags=r16(parent+2);owner=r32(parent+4);token_frame=r64(parent+24);
            if(parent[0]!=2||parent[1]!=depth||(parent_flags&~1u)||((parent_flags!=0)!=(owner!=0))||(!owner&&token_frame)
               ||r64(parent+8)!=seq+1||seq+1>=SEQ_LIMIT||(depth==0)!=(parent_id==0))return PFR_INVALID;
            if(l->depth){stack_node *top=&ns[l->stack];if(depth!=(unsigned)top->begin[1]+1||parent_id!=r64(top->begin+24))return PFR_INVALID;}
            else if(!(l->flags&16)&&depth)return PFR_INVALID;
            else if(!depth)l->flags&=~UINT32_C(16);
            if(owner){rc=remember_token(c,s,owner,token_frame);if(rc!=PFR_OK)return rc;}
            if(l->depth>=256||s->free_stack==NONE)return PFR_STATE_BYTES;
            node=s->free_stack;s->free_stack=ns[node].next;memset(&ns[node],0,sizeof ns[node]);ns[node].used=1;
            ns[node].next=l->stack;memcpy(ns[node].begin,r,32);memcpy(ns[node].parent,parent,32);
            ns[node].context=(l->flags&8)?2:(l->flags&2)?1:0;ns[node].affected=(l->flags&16)!=0;
            ns[node].frame=ns[node].context==1?l->frame:0;l->stack=node;++l->depth;++s->stacks_used;
            l->max_span=id;++i;++l->next;++l->written;++s->total_written;
        }else if(kind==2)return PFR_INVALID;
        else if(kind==3){uint32_t node=l->stack;
            if((flags&~1u)||!id||!have_desc(c,s,desc))return PFR_INVALID;
            if(!l->depth){if(!(l->flags&16))return PFR_INVALID;}
            else{
                if(node==NONE||depth!=ns[node].begin[1]||desc!=r32(ns[node].begin+4)||id!=r64(ns[node].begin+24))return PFR_INVALID;
                pop_stack(c,s,l);
            }
        }else if(kind==4){uint32_t node=l->frame_tail;
            if(flags||depth||desc||(id!=l->next_frame&&(!(l->flags&8)||id<l->next_frame))||id>seq)return PFR_INVALID;
            if(node==NONE||fs[node].last+1!=id){uint32_t previous=node;
                if(s->free_frame==NONE)return PFR_IDENTITIES;
                node=s->free_frame;s->free_frame=fs[node].next;memset(&fs[node],0,sizeof fs[node]);fs[node].used=1;fs[node].next=NONE;
                fs[node].first=id;if(previous==NONE)l->frame_head=node;else fs[previous].next=node;
                l->frame_tail=node;++l->frame_runs;++s->frames_used;
            }
            fs[node].last=id;l->frame=id;l->next_frame=id+1;l->last_frame_seq=seq;l->last_frame_tb=tb;l->flags=(l->flags&~UINT32_C(8))|2|64;
            resolve_token(c,s,l->inc,id);
        }else{
            if(flags||depth||(desc!=1&&desc!=2))return PFR_INVALID;
            ++l->regerr;
        }
        if(kind!=6){
            if((l->flags&1)&&tb<l->last_tb)l->flags|=32;
            if(!(l->flags&1)||tb>l->last_tb)l->last_tb=tb;
            l->flags|=1;
        }
        ++l->next;++l->written;++s->total_written;
    }return done(p)?PFR_OK:PFR_INVALID;
}
static int final_entry(const context *c,state *s,cursor *p,int retired)
{
    uint32_t inc=c32(p);unsigned status=c8(p),flags=c8(p);uint16_t reserved=c16(p);
    uint64_t produced=c64(p),written=c64(p),dropped=c64(p),refused=c64(p),regerr=c64(p),open=c64(p);lane *l=find_lane(c,s,inc);
    if(p->bad||!l||l->final_seen||status<1||status>3||(retired&&status!=1)||reserved||(flags&~3u)
       ||written>SEQ_LIMIT||written!=l->written)return PFR_INVALID;
    if(status==3){
        if(flags!=2||produced!=UINT64_MAX||dropped!=UINT64_MAX||refused!=UINT64_MAX||regerr!=UINT64_MAX||open!=UINT64_MAX)return PFR_INVALID;
    }else{
        if((flags&2)||produced>SEQ_LIMIT||dropped>SEQ_LIMIT||refused>SEQ_LIMIT||regerr>SEQ_LIMIT||open>256
           ||produced<written||produced-written!=dropped||produced<l->next
           ||dropped<l->gaps||dropped-l->gaps!=produced-l->next||regerr<l->regerr)return PFR_INVALID;
    }
    l->final_seen=1;l->final_state=status;l->final_limits=flags;l->final_dropped=dropped;
    if(retired)l->flags|=128;
    return PFR_OK;
}
static int retired_chunk(const context *c,state *s,cursor *p)
{
    uint32_t count=c32(p),i;int rc;
    if(p->bad||!count||(uint64_t)count*56!=p->n-p->at)return PFR_INVALID;
    for(i=0;i<count;++i){rc=final_entry(c,s,p,1);if(rc!=PFR_OK)return rc;}
    return done(p)?PFR_OK:PFR_INVALID;
}
static int trailer_chunk(const context *c,state *s,cursor *p)
{
    uint32_t end_flags=c32(p),count=c32(p),i;uint64_t sink,descriptor_count;uint32_t reserved;int rc,any_state3=0,any_limit=0;
    lane *ls=lanes(c,s);token *ts=tokens(c,s);
    if(p->bad||(end_flags&~7u)||(uint64_t)count*56+32!=p->n-p->at)return PFR_INVALID;
    for(i=0;i<count;++i){rc=final_entry(c,s,p,0);if(rc!=PFR_OK)return rc;}
    sink=c64(p);(void)c32(p);reserved=c32(p);(void)c64(p);descriptor_count=c64(p);
    if(!done(p)||reserved||descriptor_count!=s->descriptor_count)return PFR_INVALID;
    for(i=0;i<s->lane_count;++i)if(ls[i].final_seen){
        if(ls[i].final_state==3)any_state3=1;
        else if(ls[i].final_limits&1)any_limit=1;
    }
    if(((end_flags&1)!=0)!=any_state3||((end_flags&2)!=0)!=any_limit||((end_flags&4)!=0)!=(sink!=0))return PFR_INVALID;
    /* The token obligation includes closed/abandoned spans. A lossless,
     * quiesced final owner cannot name a frame never actually recorded. */
    for(i=0;i<s->tokens_used;++i){lane *l=find_lane(c,s,ts[i].owner);
        if(!l)return PFR_INVALID;
        if(frame_member(c,s,l,ts[i].frame)||!l->final_seen||l->final_state==3||l->gap_count||l->final_dropped)continue;
        return PFR_INVALID;
    }
    /* Missing finals and open windows remain incomplete evidence in Rust;
     * a genuine terminal chunk does not fabricate those missing assertions. */
    s->terminal=1;return PFR_OK;
}
static int window_begin(const context *c,state *s,cursor *p)
{
    unsigned schema=c16(p),flags=c16(p);uint32_t id=c32(p),name=c32(p);uint64_t first=c64(p);
    unsigned source=c8(p),kind=c8(p),reserved=c16(p);uint32_t count=c32(p),reserved2=c32(p);
    if(p->n!=32||!done(p)||schema!=1||flags||reserved||reserved2||s->window_open||!id||id<=s->window_last_id
       ||(s->window_previous&&first<=s->window_last_frame)||(source!=1&&source!=2)||kind<1||kind>3
       ||(kind==2 ? !count : count!=0))return PFR_INVALID;
    if(name==UINT32_MAX){if(source!=1||kind!=1)return PFR_INVALID;}
    else if(!have_desc(c,s,name))return PFR_INVALID;
    s->window_open=1;s->window_id=id;s->window_kind=kind;s->window_count=count;s->window_first=first;return PFR_OK;
}
static int window_end(state *s,cursor *p)
{
    unsigned schema=c16(p),flags=c16(p);uint32_t id=c32(p);unsigned reason=c8(p),r0=c8(p),r1=c16(p);
    uint64_t last=c64(p);const uint8_t *reserved=take(p,12);unsigned i;
    if(p->n!=32||!done(p)||schema!=1||flags||r0||r1||!s->window_open||id!=s->window_id
       ||last<s->window_first||reason<1||reason>5)return PFR_INVALID;
    for(i=0;i<12;++i)if(reserved[i])return PFR_INVALID;
    if(reason==2){uint64_t difference=last-s->window_first;
        if(s->window_kind!=2||difference==UINT64_MAX||difference+1!=s->window_count)return PFR_INVALID;
    }
    s->window_open=0;s->window_last_id=id;s->window_previous=1;s->window_last_frame=last;return PFR_OK;
}
int pfp_core_accept(void *opaque,const void *accepted,void *prepared,const void *chunk,size_t n,
    uint32_t external_metadata,pfo_prepared *result)
{
    const context *c=opaque;const state *a=accepted;state *s=prepared;const uint8_t *raw=chunk;
    cursor p;uint32_t ty,len;int rc;
    if(result)memset(result,0,sizeof *result);
    if(!c||c->magic!=CONTEXT_MAGIC||!a||!s||a==s||!chunk||!result
       ||(external_metadata!=0&&!(external_metadata>=8&&external_metadata<=13)
          &&external_metadata!=15&&external_metadata!=16&&external_metadata!=17&&external_metadata!=18)
       ||(uintptr_t)a%c->layout.arena_alignment||(uintptr_t)s%c->layout.arena_alignment
       ||a->magic!=ARENA_MAGIC||a->arena_bytes!=c->layout.arena_bytes||a->terminal||n<20)return PFR_INVALID;
    ty=r32(raw);len=r32(raw+4);
    if(len>MAX_CHUNK||(uint64_t)len+20!=n||r64(raw+8)!=a->next||crc(raw,n-4)!=r32(raw+n-4)
       ||!((ty>=1&&ty<=7)||((r32(a->header+12)&512)&&(ty==41||ty==42))||(external_metadata&&ty==external_metadata))
       ||(a->next==0&&ty!=1)||(a->next==1&&ty!=2)||(a->next>=2&&(ty==1||ty==2)))return PFR_INVALID;
    if(a->next==UINT64_MAX-1||n>UINT64_MAX-16-a->bytes){result->reason=PFR_COUNTER_RANGE;return PFO_RESOURCE;}
    memcpy(s,a,c->layout.arena_bytes);
    if(ty!=5&&ty!=13){
        if(n>c->cfg.metadata_bytes||s->metadata_used>c->cfg.metadata_bytes-n||4>c->cfg.metadata_bytes-n-s->metadata_used){
            result->reason=PFR_SUPPORT;return PFO_RESOURCE;
        }
        if(ty!=6){uint8_t *m=(uint8_t *)s+c->layout.metadata_offset+s->metadata_used;w32(m,(uint32_t)n);memcpy(m+4,raw,n);
            s->metadata_used+=(uint32_t)n+4;++s->metadata_count;
        }
    }
    p.p=raw+16;p.n=len;p.at=0;p.bad=0;
    switch(ty){
    case 1:rc=header_chunk(c,s,&p);break;
    case 2:rc=clocks_chunk(s,&p);break;
    case 3:rc=descriptors_chunk(c,s,&p);break;
    case 4:rc=threads_chunk(c,s,&p);break;
    case 5:rc=records_chunk(c,s,&p);break;
    case 6:rc=trailer_chunk(c,s,&p);break;
    case 7:rc=retired_chunk(c,s,&p);break;
    case 8:case 9:case 10:case 11:case 12:case 13:case 15:case 16:case 17:case 18:
        rc=external_metadata==ty?PFR_OK:PFR_INVALID;break;
    case 41:rc=window_begin(c,s,&p);break;
    case 42:rc=window_end(s,&p);break;
    default:rc=PFR_INVALID;break;
    }
    if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}if(rc!=PFR_OK)return rc;
    /* Canonical joint identity occupancy belongs to this whole successor,
     * after all same-chunk FRAME resolutions. Individual pools never grow. */
    if(s->tokens_used>c->cfg.frame_identities||s->frames_used>c->cfg.frame_identities-s->tokens_used){
        result->reason=PFR_IDENTITIES;return PFO_RESOURCE;
    }
    ++s->next;s->bytes+=n;
    result->terminal=s->terminal;return PFR_OK;
}
int pfp_prepare(void *opaque,const void *accepted,void *prepared,const void *chunk,size_t n,
    void *checkpoint,size_t capacity,pfo_prepared *result)
{
    const context *c=opaque;size_t cp_bytes=0;int rc;
    if(result)memset(result,0,sizeof *result);
    if(!c||c->magic!=CONTEXT_MAGIC||!checkpoint||!result||capacity<c->cfg.checkpoint_bytes)return PFR_INVALID;
    rc=pfp_core_accept(opaque,accepted,prepared,chunk,n,0,result);if(rc!=PFR_OK)return rc;
    if(!result->terminal){rc=encode(c,prepared,checkpoint,capacity,&cp_bytes);
        if(rc>0){result->reason=(uint32_t)rc;return PFO_RESOURCE;}if(rc!=PFR_OK)return rc;
    }
    result->checkpoint_bytes=cp_bytes;return PFR_OK;
}
int pfp_core_view(const void *opaque,const void *arena,pfp_view *v)
{
    const context *c=opaque;const state *s=arena;const uint8_t *m;size_t first;uint32_t i;
    if(!c||c->magic!=CONTEXT_MAGIC||!s||!v||s->magic!=ARENA_MAGIC||s->arena_bytes!=c->layout.arena_bytes)return PFR_INVALID;
    memset(v,0,sizeof *v);v->header=s->header;v->metadata=(const uint8_t *)s+c->layout.metadata_offset;
    v->next=s->next;v->bytes=s->bytes;v->metadata_bytes=s->metadata_used;v->metadata_count=s->metadata_count;
    v->lanes=s->lane_count;v->stacks=s->stacks_used;v->frames=s->frames_used;v->tokens=s->tokens_used;v->terminal=s->terminal;
    for(i=0;i<s->lane_count;++i){const lane *l=&lanes(c,(void *)s)[i];
        if(l->written>UINT64_MAX-v->physical_rows)return PFR_INVALID;
        v->physical_rows+=l->written;if(l->gap_count>UINT64_MAX-v->physical_rows)return PFR_INVALID;
        v->physical_rows+=l->gap_count;
    }
    /* Actual CLOCKS is the second metadata entry. No copied clock or header. */
    if(s->have_clock){m=v->metadata;if(v->metadata_bytes<4)return PFR_INVALID;
        first=(size_t)r32(m)+4;if(first>v->metadata_bytes||v->metadata_bytes-first<52)return PFR_INVALID;
        if(r32(m+first)!=48||r32(m+first+4)!=2)return PFR_INVALID;
        v->clock_frequency=r64(m+first+4+16+12);
    }
    return PFR_OK;
}
