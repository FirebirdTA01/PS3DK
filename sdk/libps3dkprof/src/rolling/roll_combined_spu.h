/* Private implementation included by roll_combined.c. All arena references
 * are indices/offsets; no pointers survive prepare or arena relocation.
 * Metadata remains in the shared PPU arena. This retains no raw event history
 * beyond bounded active BEGIN witnesses. Schema 1 is causal support only. */
#define SP_NONE UINT32_MAX
typedef struct { uint32_t id,ref,lane,epoch,kind,bound,final; uint64_t cal,last_tb; } sp_domain;
typedef struct { uint32_t id,descs; } sp_image;
typedef struct {
    uint64_t seq; uint32_t epoch,raw,desc,depth,next,used;
} sp_node;
typedef struct {
    uint32_t id,domain,image,known,ever_unknown,kept_epoch,final,depth,head;
    uint32_t protocol[6],order_any,order_final,bracket_epoch,bracket_seq;
    uint64_t next,events,lost,decoded,drained,before;
} sp_lane;
typedef struct { uint32_t kind,ordinal,head,count; uint64_t first,last; } sp_entry;
typedef struct {
    uint32_t lane,epoch,known,depth,head,next,protocol[6];
    uint64_t first,events,chunks;
} sp_run;
typedef struct {
    uint32_t domains,spu_domains,rsx_domains,images,lanes,entries,runs,witnesses,live,epochs,descs,losses;
    uint64_t brackets,orders,chunks,events;
} sp_state;
typedef struct { uint32_t bytes,domains,images,lanes,entries,runs,live,saved,desc_scratch,rsx_capacity,mixed; } sp_layout;
typedef struct { const uint8_t *p;size_t n,at;int bad; } sp_cursor;
static const uint8_t *sp_take(sp_cursor *c,size_t n)
{static const uint8_t zero[32]={0};if(c->bad||n>c->n-c->at){c->bad=1;return zero;}c->at+=n;return c->p+c->at-n;}
static uint32_t sp_u32(sp_cursor *c){return r32(sp_take(c,4));}
static uint64_t sp_u64(sp_cursor *c){return r64(sp_take(c,8));}
static int sp_utf8(const uint8_t *p,size_t n)
{
    size_t i=0;while(i<n){uint32_t ch=p[i++],value,min;unsigned more,j;
        if(!ch)return 0;
        if(ch<128)continue;
        if(ch>=194&&ch<=223){more=1;value=ch&31;min=128;}
        else if(ch>=224&&ch<=239){more=2;value=ch&15;min=2048;}
        else if(ch>=240&&ch<=244){more=3;value=ch&7;min=65536;}
        else return 0;
        if(more>n-i)return 0;
        for(j=0;j<more;++j){ch=p[i++];if((ch&192)!=128)return 0;
            value=(value<<6)|(ch&63);}
        if(value<min||value>1114111||(value>=55296&&value<=57343))return 0;
    }return 1;
}
static uint32_t sp_string(sp_cursor *c,int binary)
{uint32_t n=r16(sp_take(c,2));const uint8_t *p=sp_take(c,n);if(c->bad||n>512||(!binary&&!sp_utf8(p,n)))c->bad=1;return n;}
static int sp_part(size_t *n,uint32_t count,size_t size,uint32_t *offset)
{if(!align8(n))return 0;*offset=(uint32_t)*n;return mul(n,count,size)&&fit(*n);}
static int sp_plan(const pfm_config *c,uint32_t rsx,uint32_t mixed,sp_layout *l)
{
    size_t n=sizeof(sp_state);memset(l,0,sizeof *l);
    if(rsx>64||rsx>UINT32_MAX-c->spu_domains)return 0;
    l->rsx_capacity=rsx;l->mixed=mixed;
    if(!sp_part(&n,c->spu_domains+rsx,sizeof(sp_domain),&l->domains)
       ||!sp_part(&n,c->spu_images,sizeof(sp_image),&l->images)
       ||!sp_part(&n,c->spu_lanes,sizeof(sp_lane),&l->lanes)
       ||!sp_part(&n,c->journal_entries,sizeof(sp_entry),&l->entries)
       ||!sp_part(&n,c->event_runs,sizeof(sp_run),&l->runs)
       ||!sp_part(&n,c->live_spu_stack_rows,sizeof(sp_node),&l->live)
       ||!sp_part(&n,c->saved_spu_witnesses,sizeof(sp_node),&l->saved)
       ||!sp_part(&n,c->image_descriptors,sizeof(uint32_t),&l->desc_scratch)||!align8(&n))return 0;
    l->bytes=(uint32_t)n;return 1;
}
#define SP_ARRAY(s,l,field,type) ((type *)((uint8_t *)(s)+(l)->field))
static sp_domain *sp_domain_find(sp_state *s,const sp_layout *l,uint32_t id)
{uint32_t i;sp_domain *a=SP_ARRAY(s,l,domains,sp_domain);for(i=0;i<s->domains;++i)if(a[i].id==id)return &a[i];return NULL;}
static sp_lane *sp_lane_find(sp_state *s,const sp_layout *l,uint32_t id)
{uint32_t i;sp_lane *a=SP_ARRAY(s,l,lanes,sp_lane);for(i=0;i<s->lanes;++i)if(a[i].id==id)return &a[i];return NULL;}
static sp_image *sp_image_find(sp_state *s,const sp_layout *l,uint32_t id)
{uint32_t i;sp_image *a=SP_ARRAY(s,l,images,sp_image);for(i=0;i<s->images;++i)if(a[i].id==id)return &a[i];return NULL;}
static int sp_owner(const pfp_view *v,uint32_t owner)
{
    size_t at=0;uint32_t i;if(!owner)return 1;
    for(i=0;i<v->metadata_count;++i){const uint8_t *raw;uint32_t n,count,j;sp_cursor p;
        if(v->metadata_bytes-at<4)return 0;
        n=r32(v->metadata+at);at+=4;if(n>v->metadata_bytes-at||n<20)return 0;
        raw=v->metadata+at;at+=n;if(r32(raw)!=4)continue;
        p.p=raw+16;p.n=n-20;p.at=0;p.bad=0;count=sp_u32(&p);
        for(j=0;j<count;++j){uint32_t inc=sp_u32(&p);(void)sp_u32(&p);(void)sp_u64(&p);(void)sp_string(&p,0);
            if(p.bad)return 0;
            if(inc==owner)return 1;
        }
    }return 0;
}
static void sp_pop(sp_state *s,const sp_layout *l,sp_lane *lane)
{sp_node *a=SP_ARRAY(s,l,live,sp_node);uint32_t n=lane->head;lane->head=a[n].next;a[n].used=0;--lane->depth;--s->live;}
static void sp_unknown(sp_state *s,const sp_layout *l,sp_lane *lane)
{while(lane->depth)sp_pop(s,l,lane);lane->known=0;lane->ever_unknown=1;}
static int sp_push(sp_state *s,const sp_layout *l,const pfm_config *c,sp_lane *lane,
                   uint64_t seq,uint32_t epoch,uint32_t raw,uint32_t desc,uint32_t depth)
{
    sp_node *a=SP_ARRAY(s,l,live,sp_node);uint32_t i;
    if(s->live==c->live_spu_stack_rows)return PFR_STATE_BYTES;
    for(i=0;i<c->live_spu_stack_rows&&a[i].used;++i){}
    if(i==c->live_spu_stack_rows)return PFR_INVALID;
    memset(&a[i],0,sizeof a[i]);a[i].seq=seq;a[i].epoch=epoch;a[i].raw=raw;a[i].desc=desc;a[i].depth=depth;
    a[i].next=lane->head;a[i].used=1;lane->head=i;++lane->depth;++s->live;return PFR_OK;
}
static int sp_event(sp_state *s,const sp_layout *l,const pfm_config *c,sp_lane *lane,
                    uint64_t seq,uint32_t epoch,const uint8_t *p)
{
    uint32_t raw=r32(p),desc=r16(p+4),kind=p[6],depth=p[7];sp_node *a=SP_ARRAY(s,l,live,sp_node);
    if(lane->protocol[0])return PFR_OK;
    if(!lane->known){if(kind==1&&depth==0){lane->known=1;return sp_push(s,l,c,lane,seq,epoch,raw,desc,depth);}return PFR_OK;}
    if(kind==1||kind==3){if(depth!=lane->depth){lane->protocol[0]=kind==1?1:2;lane->protocol[2]=depth;lane->protocol[5]=lane->depth;}
        else if(kind==1)return sp_push(s,l,c,lane,seq,epoch,raw,desc,depth);
    }else if(lane->depth){sp_node *top=&a[lane->head];if(top->desc==desc&&top->depth==depth)sp_pop(s,l,lane);
        else{lane->protocol[0]=3;lane->protocol[1]=desc;lane->protocol[2]=depth;lane->protocol[3]=top->desc;lane->protocol[4]=top->depth;}
    }else{lane->protocol[0]=4;lane->protocol[1]=desc;lane->protocol[2]=depth;}
    return PFR_OK;
}
static int sp_domains(sp_state *s,const sp_layout *l,const pfm_config *c,sp_cursor *p)
{
    uint32_t count=sp_u32(p),i,j;sp_domain *a=SP_ARRAY(s,l,domains,sp_domain);
    if(!count||count>64)return PFR_INVALID;
    for(i=0;i<count;++i){sp_domain d;const uint8_t *k;uint32_t source;uint64_t reserved=0;memset(&d,0,sizeof d);
        d.id=sp_u32(p);k=sp_take(p,4);source=sp_u32(p);d.ref=sp_u32(p);
        for(j=0;j<4;++j)reserved|=sp_u64(p);
        (void)sp_string(p,0);
        if(p->bad||d.id<2||d.id>65535||k[3]||reserved
           ||sp_domain_find(s,l,d.id))return PFR_INVALID;
        d.kind=k[0];
        if(d.kind==1){
            if(k[1]!=32||k[2]!=1||source!=1||!d.ref)return PFR_INVALID;
            for(j=0;j<s->domains;++j)if(a[j].kind==1&&a[j].ref==d.ref)return PFR_INVALID;
            if(s->spu_domains==1024)return PFR_INVALID;
            if(s->spu_domains==c->spu_domains)return PFR_IDENTITIES;
            ++s->spu_domains;
        }else if(d.kind==2){
            if(!l->mixed||k[1]!=64||k[2]!=2||source!=2||d.ref||s->rsx_domains==64)return PFR_INVALID;
            if(s->rsx_domains==l->rsx_capacity)return PFR_IDENTITIES;
            ++s->rsx_domains;
        }else return PFR_INVALID;
        a[s->domains++]=d;
    }return PFR_OK;
}
static int sp_images(sp_state *s,const sp_layout *l,const pfm_config *c,sp_cursor *p)
{
    uint32_t count=sp_u32(p),i,j,k,*seen=SP_ARRAY(s,l,desc_scratch,uint32_t);sp_image *a=SP_ARRAY(s,l,images,sp_image);
    if(!count||count>32)return PFR_INVALID;
    for(i=0;i<count;++i){sp_image image;uint32_t build;image.id=sp_u32(p);build=sp_string(p,1);image.descs=sp_u32(p);
        if(p->bad||!build||build>64||!image.descs||image.descs>4096||sp_image_find(s,l,image.id)||s->images==32)return PFR_INVALID;
        if(s->images==c->spu_images||image.descs>c->image_descriptors-s->descs)return PFR_IDENTITIES;
        for(j=0;j<image.descs;++j){uint32_t desc=sp_u32(p),name;(void)sp_u32(p);(void)sp_u32(p);name=sp_string(p,0);(void)sp_string(p,0);
            if(p->bad||desc>=image.descs||!name)return PFR_INVALID;
            for(k=0;k<j;++k)if(seen[k]==desc)return PFR_INVALID;
            seen[j]=desc;
        }
        /* Scratch is canonical, so repeated dirty-arena prepares agree. */
        memset(seen,0,(size_t)image.descs*sizeof *seen);s->descs+=image.descs;a[s->images++]=image;
    }return PFR_OK;
}
static int sp_lanes(sp_state *s,const sp_layout *l,const pfm_config *c,sp_cursor *p,const pfp_view *v)
{
    uint32_t count=sp_u32(p),i,j;sp_lane *a=SP_ARRAY(s,l,lanes,sp_lane);
    if(!count||count>64)return PFR_INVALID;
    for(i=0;i<count;++i){sp_lane lane;sp_domain *d;uint32_t owner,live=0;memset(&lane,0,sizeof lane);
        lane.id=sp_u32(p);lane.domain=sp_u32(p);lane.image=sp_u32(p);owner=sp_u32(p);(void)sp_u64(p);(void)sp_string(p,0);
        d=sp_domain_find(s,l,lane.domain);
        if(p->bad||!lane.id||sp_lane_find(s,l,lane.id)||!d||d->kind!=1||d->ref!=lane.id||d->lane
           ||!sp_image_find(s,l,lane.image)||!sp_owner(v,owner))return PFR_INVALID;
        for(j=0;j<s->lanes;++j)if(!a[j].final)++live;
        if(s->lanes==1024||live==64)return PFR_INVALID;
        if(s->lanes==c->spu_lanes)return PFR_IDENTITIES;
        d->lane=lane.id;lane.known=1;lane.head=SP_NONE;a[s->lanes++]=lane;
    }return PFR_OK;
}
static int sp_epochs(sp_state *s,const sp_layout *l,const pfm_config *c,sp_cursor *p)
{
    uint32_t count=sp_u32(p),i;if(!count||count>256)return PFR_INVALID;
    for(i=0;i<count;++i){uint32_t id=sp_u32(p),epoch=sp_u32(p);const uint8_t *r=sp_take(p,4);uint64_t written=sp_u64(p);
        sp_domain *d=sp_domain_find(s,l,id);sp_lane *lane=d?sp_lane_find(s,l,d->lane):NULL;(void)sp_u64(p);
        if(p->bad||!d||epoch!=d->epoch+1||r[1]||r[2]||r[3]
           ||(epoch==1?r[0]!=1:(r[0]!=2&&r[0]!=5&&r[0]!=6))||(r[0]==2?written>UINT32_MAX:written!=0))return PFR_INVALID;
        if(d->kind==2){if(d->final||epoch!=1||r[0]!=1||written)return PFR_INVALID;}
        else if(!lane||lane->final)return PFR_INVALID;
        if(s->epochs==4096)return PFR_INVALID;
        if(s->epochs==c->epochs)return PFR_SUPPORT;
        ++s->epochs;d->epoch=epoch;d->cal=0;d->last_tb=0;
        if(d->kind==1&&epoch>1){if(r[0]==2&&lane->known&&!lane->ever_unknown&&!lane->protocol[0]&&!lane->depth)lane->kept_epoch=1;
            else sp_unknown(s,l,lane);}
    }return PFR_OK;
}
static int sp_cal(sp_state *s,const sp_layout *l,const pfm_config *c,sp_cursor *p)
{
    uint32_t id=sp_u32(p),epoch=sp_u32(p),count,i;uint64_t first=sp_u64(p);sp_domain *d=sp_domain_find(s,l,id);
    sp_lane *lane=d?sp_lane_find(s,l,d->lane):NULL;count=sp_u32(p);
    if(p->bad||!count||count>43689||(uint64_t)count*24!=p->n-p->at||!lane||lane->final||!epoch||epoch!=d->epoch
       ||first!=d->cal||count>65536-first||count>262144-s->brackets)return PFR_INVALID;
    if(count>c->brackets-s->brackets)return PFR_SUPPORT;
    for(i=0;i<count;++i){uint64_t raw=sp_u64(p),tb0=sp_u64(p),tb1=sp_u64(p);
        if(raw>UINT32_MAX||tb0>=tb1||((first||i)&&tb0<d->last_tb))return PFR_INVALID;
        d->last_tb=tb1;
    }d->cal+=count;s->brackets+=count;return PFR_OK;
}
static int sp_order(sp_state *s,const sp_layout *l,sp_cursor *p,uint32_t flags)
{
    uint32_t n=sp_u32(p),reserved=sp_u32(p),kind,id,k2=0,id2=0;uint64_t lo,hi,lo2=0,hi2=0;sp_lane *lane;sp_domain *d;
    if(!(flags&4)||reserved||!n||n>2||(uint64_t)n*24!=p->n-p->at)return PFR_INVALID;
    kind=sp_u32(p);id=sp_u32(p);lo=sp_u64(p);hi=sp_u64(p);
    if(n==2){k2=sp_u32(p);id2=sp_u32(p);lo2=sp_u64(p);hi2=sp_u64(p);}
    if((n==2&&(kind!=2||k2!=3||id!=id2||hi!=hi2))||(n==1&&kind!=1&&kind!=4&&kind!=5))return PFR_INVALID;
    lane=sp_lane_find(s,l,id);if(!lane||lane->final||lane->order_final||n>1048576-s->orders)return PFR_INVALID;
    d=sp_domain_find(s,l,lane->domain);
    if(kind==4){if(hi||lane->order_any)return PFR_INVALID;
        ++lane->decoded;}
    else if(kind==2){if((lo>>32)||!lo||lo!=d->epoch||hi!=lane->next||hi<=lane->drained)return PFR_INVALID;
        lane->drained=hi;lane->decoded+=2;(void)lo2; /* Original DRAIN_TB remains in support, including decreases. */
    }else if(kind==1){uint32_t epoch=(uint32_t)lo,seq=(uint32_t)(lo>>32);
        if(!epoch||epoch!=d->epoch||seq>=d->cal||(lane->bracket_epoch==epoch&&seq<=lane->bracket_seq)
           ||hi<lane->drained||hi>lane->next||hi<lane->before)return PFR_INVALID;
        lane->bracket_epoch=epoch;lane->bracket_seq=seq;lane->before=hi;++lane->decoded;
    }else{if(lo!=lane->decoded)return PFR_INVALID;
        lane->order_final=1;}
    lane->order_any=1;s->orders+=n;return PFR_OK;
}
static int sp_loss(sp_state *s,const sp_layout *l,sp_cursor *p)
{
    uint32_t id=sp_u32(p),reserved;const uint8_t *r=sp_take(p,4);uint64_t first=sp_u64(p),count=sp_u64(p);
    sp_lane *lane=sp_lane_find(s,l,id);reserved=sp_u32(p);
    if(p->bad||r[0]!=1||r[1]<1||r[1]>4||r[2]||r[3]||reserved||!count||!lane||lane->final||first!=lane->next
       ||count>UINT64_MAX-first||count>UINT64_MAX-lane->lost||s->losses==65536)return PFR_INVALID;
    ++s->losses;lane->next+=count;lane->lost+=count;sp_unknown(s,l,lane);return PFR_OK;
}
static int sp_final(sp_state *s,const sp_layout *l,sp_cursor *p,uint32_t flags)
{
    uint32_t count=sp_u32(p),i;if(!count||count>128)return PFR_INVALID;
    for(i=0;i<count;++i){uint32_t id=sp_u32(p),open;const uint8_t *r=sp_take(p,4);
        uint64_t next=sp_u64(p),events=sp_u64(p),lost=sp_u64(p);sp_lane *lane=sp_lane_find(s,l,id);open=sp_u32(p);
        if(p->bad||r[1]<1||r[1]>3||r[2]||r[3])return PFR_INVALID;
        if(r[0]==2){sp_domain *d=sp_domain_find(s,l,id);
            if(!d||d->kind!=2||d->final||!d->bound||next||events||lost||open)return PFR_INVALID;
            d->final=r[1];continue;
        }
        if(r[0]!=1||!lane||lane->final
           ||((flags&4)&&!lane->order_final)||events>UINT64_MAX-lost||next!=events+lost
           ||next!=lane->next||events!=lane->events||lost!=lane->lost)return PFR_INVALID;
        if(!lane->ever_unknown&&!lane->protocol[0]&&!(lane->kept_epoch&&open==UINT32_MAX)&&open!=lane->depth)return PFR_INVALID;
        lane->final=r[1];
    }return PFR_OK;
}
/* Snapshot only the ending open stack. Closed runs retain their bounded
 * witnesses; the current run's old witnesses are reclaimed on replacement. */
static int sp_snapshot(sp_state *s,const sp_layout *l,const pfm_config *c,sp_run *run,const sp_lane *lane)
{
    sp_node *saved=SP_ARRAY(s,l,saved,sp_node),*live=SP_ARRAY(s,l,live,sp_node);uint32_t i,node,previous=SP_NONE;
    if(lane->depth>c->saved_spu_witnesses-(s->witnesses-run->depth))return PFR_STATE_BYTES;
    node=run->head;for(i=0;i<run->depth;++i){uint32_t next=saved[node].next;saved[node].used=0;node=next;}
    s->witnesses-=run->depth;run->head=SP_NONE;run->depth=0;node=lane->head;
    while(node!=SP_NONE){uint32_t slot;for(slot=0;slot<c->saved_spu_witnesses&&saved[slot].used;++slot){}
        if(slot==c->saved_spu_witnesses)return PFR_INVALID;
        saved[slot]=live[node];saved[slot].next=SP_NONE;
        if(previous==SP_NONE)run->head=slot;else saved[previous].next=slot;
        previous=slot;node=live[node].next;++run->depth;++s->witnesses;
    }
    run->known=lane->known;memcpy(run->protocol,lane->protocol,sizeof run->protocol);return PFR_OK;
}
static int sp_events(sp_state *s,const sp_layout *l,const pfm_config *c,sp_cursor *p,uint64_t index)
{
    uint32_t id=sp_u32(p),epoch=sp_u32(p),count,i,node,previous=SP_NONE;uint64_t first=sp_u64(p);
    sp_lane *lane=sp_lane_find(s,l,id);sp_domain *d=lane?sp_domain_find(s,l,lane->domain):NULL;
    sp_image *image=lane?sp_image_find(s,l,lane->image):NULL;sp_entry *entries=SP_ARRAY(s,l,entries,sp_entry),*entry;
    sp_run *runs=SP_ARRAY(s,l,runs,sp_run),*run;int rc;count=sp_u32(p);
    if(p->bad||!count||count>131069||(uint64_t)count*8!=p->n-p->at||!lane||lane->final||!epoch||epoch!=d->epoch
       ||first!=lane->next||count>UINT64_MAX-first)return PFR_INVALID;
    if(count>UINT64_MAX-lane->events||count>UINT64_MAX-s->events||s->chunks==UINT64_MAX)return PFR_COUNTER_RANGE;
    for(i=0;i<count;++i){const uint8_t *ev=sp_take(p,8);
        if(r16(ev+4)>=image->descs||ev[6]<1||ev[6]>3||ev[7]>63)return PFR_INVALID;
        rc=sp_event(s,l,c,lane,first+i,epoch,ev);if(rc!=PFR_OK)return rc;
    }
    if(!s->entries||entries[s->entries-1].kind!=2){
        if(s->entries==c->journal_entries)return PFR_SUPPORT;
        entry=&entries[s->entries++];memset(entry,0,sizeof *entry);entry->kind=2;entry->first=index;entry->head=SP_NONE;
    }else entry=&entries[s->entries-1];
    entry->last=index;node=entry->head;
    while(node!=SP_NONE&&runs[node].lane<id){previous=node;node=runs[node].next;}
    if(node==SP_NONE||runs[node].lane!=id){uint32_t fresh;
        if(s->runs==c->event_runs)return PFR_STATE_BYTES;
        fresh=s->runs++;run=&runs[fresh];memset(run,0,sizeof *run);run->lane=id;run->epoch=epoch;run->first=first;run->head=SP_NONE;run->next=node;
        if(previous==SP_NONE)entry->head=fresh;else runs[previous].next=fresh;
        ++entry->count;
    }else run=&runs[node];
    if(run->epoch!=epoch||run->first+run->events!=first)return PFR_INVALID;
    if(count>UINT64_MAX-run->events||run->chunks==UINT64_MAX)return PFR_COUNTER_RANGE;
    run->events+=count;++run->chunks;rc=sp_snapshot(s,l,c,run,lane);if(rc!=PFR_OK)return rc;
    lane->next+=count;lane->events+=count;s->events+=count;++s->chunks;return PFR_OK;
}
static int sp_accept(sp_state *s,const sp_layout *l,const pfm_config *c,const pfp_view *v,
                     uint32_t ty,const uint8_t *payload,size_t n,uint32_t flags)
{
    sp_cursor p={payload,n,0,0};sp_entry *entries=SP_ARRAY(s,l,entries,sp_entry);int rc;
    if(ty!=13&&s->entries==c->journal_entries)return PFR_SUPPORT;
    switch(ty){
    case 8:rc=sp_domains(s,l,c,&p);break;
    case 9:rc=sp_epochs(s,l,c,&p);break;
    case 10:rc=sp_cal(s,l,c,&p);break;
    case 11:rc=sp_images(s,l,c,&p);break;
    case 12:rc=sp_lanes(s,l,c,&p,v);break;
    case 13:rc=sp_events(s,l,c,&p,v->next);break;
    case 15:rc=sp_loss(s,l,&p);break;
    case 16:rc=sp_final(s,l,&p,flags);break;
    case 18:rc=sp_order(s,l,&p,flags);break;
    default:return PFR_INVALID;
    }
    if(rc!=PFR_OK)return rc;
    if(p.bad||p.at!=p.n)return PFR_INVALID;
    if(ty!=13){sp_entry *entry=&entries[s->entries++];memset(entry,0,sizeof *entry);
        entry->kind=1;entry->ordinal=v->metadata_count;entry->first=entry->last=v->next;}
    return PFR_OK;
}
static int sp_terminal(sp_state *s,const sp_layout *l)
{uint32_t i;sp_domain *d=SP_ARRAY(s,l,domains,sp_domain);for(i=0;i<s->domains;++i)if(d[i].kind==1&&!d[i].lane)return PFR_INVALID;return PFR_OK;}
static uint64_t sp_wire_size(const sp_state *s)
{return (uint64_t)s->entries*32+(uint64_t)s->runs*64+(uint64_t)s->witnesses*24;}
static int sp_encode(sp_state *s,const sp_layout *l,const pfp_view *v,uint8_t *out,size_t *written)
{
    uint32_t i,ordinal=0;size_t at=0,metadata_at=0;sp_entry *entries=SP_ARRAY(s,l,entries,sp_entry);sp_run *runs=SP_ARRAY(s,l,runs,sp_run);
    sp_node *nodes=SP_ARRAY(s,l,saved,sp_node);
    for(i=0;i<s->entries;++i){sp_entry *e=&entries[i];size_t start=at;uint32_t run;
        uint32_t current=0;
        if(e->kind==1){int found=0;
            while(ordinal<v->metadata_count){uint32_t n,ty;uint64_t index;const uint8_t *raw;
                if(metadata_at>v->metadata_bytes||v->metadata_bytes-metadata_at<4)return PFR_INVALID;
                n=r32(v->metadata+metadata_at);
                if(n<20||n>v->metadata_bytes-metadata_at-4)return PFR_INVALID;
                raw=v->metadata+metadata_at+4;index=r64(raw+8);ty=r32(raw);
                if(index>e->first)return PFR_INVALID;
                current=ordinal++;metadata_at+=(size_t)n+4;
                if(index==e->first){if(!((ty>=8&&ty<=12)||ty==15||ty==16||ty==18))return PFR_INVALID;found=1;break;}
            }
            if(!found)return PFR_INVALID;
        }
        w32(out+at,e->kind);w32(out+at+4,32);w64(out+at+8,e->first);w64(out+at+16,e->last);
        w32(out+at+24,e->kind==1?current:e->count);w32(out+at+28,0);at+=32;
        if(e->kind==1)continue;
        for(run=e->head;run!=SP_NONE;run=runs[run].next){sp_run *r=&runs[run];uint32_t j;
            w32(out+at,r->lane);w32(out+at+4,r->epoch);w64(out+at+8,r->first);w64(out+at+16,r->events);w64(out+at+24,r->chunks);
            w32(out+at+32,r->known);w32(out+at+36,r->depth);for(j=0;j<6;++j)w32(out+at+40+4*j,r->protocol[j]);at+=64;
            /* Linked stack is top-first; wire witnesses are bottom-first. */
            for(j=r->depth;j>0;--j){uint32_t k,node=r->head;sp_node *w;
                for(k=1;k<j;++k)node=nodes[node].next;
                w=&nodes[node];w64(out+at,w->seq);w32(out+at+8,w->epoch);w32(out+at+12,w->raw);w32(out+at+16,w->desc);w32(out+at+20,w->depth);at+=24;
            }
        }w32(out+start+4,(uint32_t)(at-start));
    }*written=at;return PFR_OK;
}
#undef SP_ARRAY
