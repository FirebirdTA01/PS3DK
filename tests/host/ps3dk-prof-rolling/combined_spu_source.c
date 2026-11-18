/* Independent original source, not an encoder test. Real SPU producer/drainer
 * behind the unchanged host memory model; test-framed ordinary PPU records.
 * Include the model once to reuse its private port shims for an owner-bearing
 * lane constructor. No model/production source is patched or duplicated. */
#define _GNU_SOURCE
#include "../ps3dk-prof-spu/model.c"
#include <inttypes.h>

typedef struct {
    wbuf source, payloads;
    uint64_t next;
    size_t collected;
    uint32_t owner_incarnation;
    FILE *provenance;
} mix_capture;
static const char mix_build[]="mixed-spu-source";
static void mix_require(int ok,const char *what)
{if(!ok){fprintf(stderr,"FAIL combined-spu-source: %s\n",what);exit(1);}}
static FILE *mix_open(const char *dir,const char *name)
{
    char path[2048];int n=snprintf(path,sizeof path,"%s/%s",dir,name);
    mix_require(n>=0&&(size_t)n<sizeof path,"path capacity");
    FILE *f=fopen(path,"wbx");mix_require(f!=NULL,"exclusive artifact creation");return f;
}
static void mix_save(const char *dir,const char *name,const void *p,size_t n)
{FILE *f=mix_open(dir,name);mix_require(fwrite(p,1,n,f)==n,"artifact write");mix_require(fclose(f)==0,"artifact close");}
static void mix_frame(mix_capture *m,uint32_t type,const wbuf *p,const char *origin,long callback)
{
    mix_require(m->next<4096&&m->source.n<1048576,"source fixture ceiling");
    mix_require(fprintf(m->provenance,"%" PRIu64 " %u %s %ld %zu\n",m->next,type,origin,callback,p->n)>0,"provenance write");
    frame(&m->source,&m->next,type,p->b,p->n);
}
static void mix_collect(mix_capture *m,const run *r)
{
    while(m->collected<r->nch){const chunk_rec *c=&r->ch[m->collected];wbuf p={c->data,c->len,c->len};
        mix_frame(m,c->type,&p,"spu-drainer",(long)m->collected);
        wle(&m->payloads,c->type,4);wle(&m->payloads,c->len,4);wb(&m->payloads,c->data,c->len);++m->collected;
    }
}
/* Same fixture construction as model lane_new, with explicit real owner
 * arguments. Every transport operation still goes through production code. */
static lane_h *mix_lane(run *r,uint32_t owner,uint32_t owner_generation)
{
    const uint32_t ls_cap=8;lane_h *h=calloc(1,sizeof *h);psp_lane *pl;
    mix_require(h!=NULL&&r->nl<MAX_LANES_H,"lane handle storage");
    mix_require(psp_lane_create(&r->p,"mixed-worker",r->image,owner,0,owner_generation,&h->idx)==PSP_P_OK,"real lane create");
    pl=&r->p.lanes[h->idx];h->lane_id=pl->lane_id;h->domain_id=pl->domain_id;h->ea_ring=pl->ea_ring;
    h->ring_bytes=(uint64_t)r->p.cfg.ring_slots*PSP_SLOT;h->pc.dec=0xFFFFFF00u;h->pc.tag_mask=0xA5u;h->pc.s=&h->s;
    h->port=(psp_port){&h->pc,p_rd_dec,p_wr_dec,p_blocking,p_rd_mask,p_wr_mask,p_tag_wait,p_getllar,p_putllc,p_putlluc,p_atomic_stat,p_put};
    h->buf=aligned_alloc(16,((size_t)ls_cap+2*PSP_HEAD)*PSP_SLOT);mix_require(h->buf!=NULL,"producer storage");
    r->lanes[r->nl++]=h;
    mix_require(psp_spu_init(&h->s,&h->port,pl->ea_ring,r->p.cfg.ring_slots,pl->ea_ctl,5,h->buf,ls_cap,1)==PSP_OK,"real SPU init");
    call_ok(h,"mixed init");return h;
}
static void mix_flush(lane_h *h,int final)
{
    uint16_t before=h->s.cal_answered;int rc=final?psp_spu_fini(&h->s):psp_spu_flush(&h->s);
    mix_require(rc==PSP_OK,final?"real fini":"real flush");call_ok(h,"mixed flush/fini");cal_note(h,before);
}
static void mix_drain(mix_capture *m,run *r)
{psp_drain(&r->p);order_ledger_ok(r,"mixed drain");mix_collect(m,r);mix_require(!r->p.sink_failed,"real sink acceptance");}
static void mix_end(mix_capture *m,run *r,lane_h *h)
{
    uint32_t open=UINT32_MAX;psp_lane_stats after;
    mix_flush(h,1);
    /* The real end wrapper requests, drains, then polls/free. Observe its
     * accepted FINAL stats between drain and poll, while storage still exists. */
    psp_lane_end_request(&r->p,h->idx);mix_drain(m,r);
    order_ledger_ok(r,"mixed end");check_stats(r,h,"mixed end stats");
    mix_require(final_open(r,h->lane_id,&open)&&open==0,"accepted closed-lane FINAL before free");
    mix_require(psp_lane_end_poll(&r->p,h->idx,1)==PSP_P_OK,"real lane end poll/free");
    mix_require(psp_lane_stats_get(&r->p,h->idx,0,&after)==PSP_P_EINVAL,"freed lane has no stats");
}
static void mix_record(wbuf *p,unsigned kind,unsigned depth,uint32_t desc,uint64_t seq,uint64_t b,uint64_t c)
{wle(p,kind,1);wle(p,depth,1);wle(p,0,2);wle(p,desc,4);wle(p,seq,8);wle(p,b,8);wle(p,c,8);}
static void mix_records(mix_capture *m,unsigned group)
{
    wbuf p={0};wle(&p,group>=3?2:1,4);wle(&p,group==2||group==4?2:3,4);
    if(group==0){mix_record(&p,4,0,0,0,1000,0);mix_record(&p,1,0,0,1,1010,1);mix_record(&p,2,0,0,2,0,0);}
    if(group==1){mix_record(&p,1,1,1,3,1020,2);mix_record(&p,2,1,0,4,1,0);mix_record(&p,3,1,1,5,1030,2);}
    if(group==2){mix_record(&p,3,0,0,6,1040,1);mix_record(&p,4,0,0,7,1100,1);}
    if(group==3){mix_record(&p,4,0,0,0,2000,0);mix_record(&p,1,0,0,1,2010,1);mix_record(&p,2,0,0,2,0,0);}
    if(group==4){mix_record(&p,3,0,0,3,2020,1);mix_record(&p,4,0,0,4,2100,1);}
    mix_frame(m,5,&p,"ppu-test-writer",-1);free(p.b);
}
static void mix_thread(mix_capture *m,uint32_t inc)
{wbuf p={0};wle(&p,1,4);wle(&p,inc,4);wle(&p,0,4);wle(&p,100+inc,8);wstr(&p,"ppu-owner");mix_frame(m,4,&p,"ppu-test-writer",-1);m->owner_incarnation=inc;free(p.b);}
static int mix_covered(void *ctx,uint32_t inc,uint32_t slot,uint32_t generation)
{const mix_capture *m=ctx;return inc&&inc==m->owner_incarnation&&slot==0&&generation==inc;}
static void mix_final_row(wbuf *p,uint32_t inc,unsigned state,uint64_t rows)
{wle(p,inc,4);wle(p,state,1);wle(p,0,1);wle(p,0,2);wle(p,rows,8);wle(p,rows,8);for(unsigned i=0;i<4;++i)wle(p,0,8);}
static uint64_t mix_hash(const uint8_t *p,size_t n)
{uint64_t v=UINT64_C(0xcbf29ce484222325);while(n--)v=(v^*p++)*UINT64_C(0x100000001b3);return v;}
static void mix_descriptors(mix_capture *m)
{
    wbuf p={0};wle(&p,2,4);
    for(unsigned i=0;i<2;++i){const char *name=i?"inner":"outer";wbuf id={0};
        wb(&id,mix_build,sizeof mix_build);wb(&id,"mixed.c",8);wle(&id,10+i,4);wb(&id,name,strlen(name));
        wle(&p,i,4);wle(&p,mix_hash(id.b,id.n),8);wle(&p,1,1);wle(&p,0,1);wle(&p,10+i,4);
        wstr(&p,name);wstr(&p,"");wstr(&p,"mixed.c");free(id.b);
    }mix_frame(m,3,&p,"ppu-test-writer",-1);free(p.b);
}
static void mix_window(mix_capture *m,int end)
{
    wbuf p={0};wle(&p,1,2);wle(&p,0,2);wle(&p,1,4);
    if(!end){wle(&p,0,4);wle(&p,0,8);wle(&p,1,1);wle(&p,1,1);wle(&p,0,2);wle(&p,0,4);wle(&p,0,4);}
    else{wle(&p,1,1);wle(&p,0,1);wle(&p,0,2);wle(&p,1,8);for(unsigned i=0;i<3;++i)wle(&p,0,4);}
    mix_frame(m,end?42:41,&p,"ppu-test-writer",-1);free(p.b);
}
static void mix_bootstrap(mix_capture *m,const run *r)
{
    wbuf p={0};wb(&m->source,"PS3DKPRF",8);wle(&m->source,1,2);wle(&m->source,7,2);wle(&m->source,519,4);
    wstr(&p,"mixed-spu-source");wstr(&p,mix_build);wstr(&p,"host-model");wstr(&p,"C11");wstr(&p,"test-only");wstr(&p,"real SPU drain");
    wle(&p,1,1);wle(&p,1,1);wle(&p,0,2);wle(&p,17,8);wle(&p,4096,4);wle(&p,2,4);wle(&p,16,4);
    mix_frame(m,1,&p,"ppu-test-writer",-1);p.n=0;
    wle(&p,1,4);wle(&p,1,4);wle(&p,0,4);wle(&p,79800000,8);wle(&p,1000,8);mix_frame(m,2,&p,"ppu-test-writer",-1);p.n=0;
    wle(&p,1,2);wle(&p,0,2);wle(&p,9,1);wb(&p,"SPU-MODEL",9);wle(&p,1,4);
    for(unsigned i=0;i<32;++i)wle(&p,0x5a,1);
    wle(&p,1689,4);wle(&p,2,4);wle(&p,7,2);
    for(unsigned i=0;i<7;++i){static const uint8_t st[]={3,3,1,3,2,2,1},width[]={4,4,8,4,1,1,40};
        wle(&p,i+1,2);wle(&p,st[i],1);wle(&p,width[i],1);
        if(i==2)wle(&p,79800000,8);
        else if(i==6){wle(&p,40,4);wle(&p,r->p.cfg.lanes_max,4);wle(&p,r->p.cfg.ring_slots,4);wle(&p,1u<<20,4);
            wle(&p,r->p.cfg.mem_max,8);for(unsigned k=0;k<4;++k)wle(&p,0,4);}
        else for(unsigned j=0;j<width[i];++j)wle(&p,0,1);
    }mix_frame(m,17,&p,"ppu-test-writer",-1);free(p.b);
}
int main(int argc,char **argv)
{
    run r;mix_capture m={0};lane_h *a,*b,*c;wbuf p={0};FILE *truth,*meta;uint16_t answered;unsigned old_slot;
    if(argc!=2){fputs("usage: combined-spu-source FRESH_ARTIFACT_DIRECTORY\n",stderr);return 2;}
    g_order_next=1;run_init(&r,64,2,NULL);mix_require(!g_failures,"model init");
    r.p.covered=mix_covered;r.p.covered_ctx=&m;
    m.provenance=mix_open(argv[1],"source.chunks");mix_bootstrap(&m,&r);mix_descriptors(&m);mix_thread(&m,1);mix_window(&m,0);mix_records(&m,0);
    a=mix_lane(&r,1,1);b=mix_lane(&r,1,1);old_slot=a->idx;mix_drain(&m,&r);
    zb(a,0);zm(b,1);mix_flush(a,0);mix_flush(b,0);mix_drain(&m,&r);
    mix_records(&m,1);zb(a,1);ze(a,1);zb(b,2);ze(b,2);mix_flush(a,0);mix_flush(b,0);mix_drain(&m,&r);
    ze(a,0);mix_flush(a,0);mix_drain(&m,&r);mix_end(&m,&r,b);
    answered=a->s.cal_answered;mix_require(psp_spu_dec_write(&a->s,UINT32_C(0xffff0000))==PSP_OK,"depth-zero epoch2");
    call_ok(a,"mixed dec_write");cal_note(a,answered);mix_flush(a,0);mix_drain(&m,&r);
    zm(a,2);mix_end(&m,&r,a);mix_records(&m,2);
    wle(&p,1,4);mix_final_row(&p,1,1,8);mix_frame(&m,7,&p,"ppu-test-writer",-1);m.owner_incarnation=0;p.n=0;
    mix_thread(&m,2);mix_records(&m,3);c=mix_lane(&r,2,2);mix_require(c->idx==old_slot,"physical slot reuse");mix_drain(&m,&r);
    zb(c,0);ze(c,0);mix_flush(c,0);mix_drain(&m,&r);mix_end(&m,&r,c);
    psp_seal(&r.p);order_ledger_ok(&r,"mixed seal");mix_collect(&m,&r);mix_records(&m,4);mix_window(&m,1);
    wle(&p,0,4);wle(&p,1,4);mix_final_row(&p,2,2,5);wle(&p,0,8);wle(&p,0,4);wle(&p,0,4);wle(&p,0,8);wle(&p,2,8);
    mix_frame(&m,6,&p,"ppu-test-writer",-1);
    mix_require(a->ntruth==5&&b->ntruth==3&&c->ntruth==2,"literal producer event counts");
    mix_require(check_streams(&r,"combined-original")==0,"independent accepted stream check");
    mix_require(check_order(&r,"combined-original",CK_RUN)==0,"independent accepted ORDER check");
    mix_require(!g_failures&&!g_order_faults&&!r.p.sink_failed,"model invariants");
    truth=mix_open(argv[1],"source.truth");
    for(unsigned i=0;i<r.nl;++i){lane_h *h=r.lanes[i];for(uint64_t q=0;q<h->ntruth;++q){tev *t=&h->truth[q];
        mix_require(fprintf(truth,"%u %" PRIu64 " %u %u %u %u\n",h->lane_id,q,t->dec,t->desc,t->kind,t->depth)>0,"truth write");}}
    mix_require(fclose(truth)==0&&fclose(m.provenance)==0,"truth/provenance close");
    mix_save(argv[1],"source.prof",m.source.b,m.source.n);mix_save(argv[1],"spu-payloads.bin",m.payloads.b,m.payloads.n);
    meta=mix_open(argv[1],"source.json");
    mix_require(fprintf(meta,"{\"schema\":1,\"flags\":519,\"ppu_rows\":13,\"ppu_spans\":3,\"spu_events\":[5,3,2],\"spu_pairs\":[2,1,1],\"spu_epochs\":[2,1,1],\"owners\":[1,1,2],\"lane_ids\":[%u,%u,%u],\"domain_ids\":[%u,%u,%u],\"slots\":[%u,%u,%u],\"image\":%u,\"chunks\":%" PRIu64 ",\"spu_callbacks\":%zu,\"source_bytes\":%zu,\"provenance\":\"real psp_spu + psp_ppu host model; test-framed PPU; no pfm\",\"profile\":\"unreviewed test declaration\"}\n",
        a->lane_id,b->lane_id,c->lane_id,a->domain_id,b->domain_id,c->domain_id,a->idx,b->idx,c->idx,r.image,m.next,m.collected,m.source.n)>0,"manifest write");
    mix_require(fclose(meta)==0,"manifest close");
    printf("PASS independent mixed original: PPU13/SPU10, 3 lanes, real transport/order checks, %" PRIu64 " chunks\n",m.next);
    free(p.b);free(m.source.b);free(m.payloads.b);run_free(&r);return 0;
}
