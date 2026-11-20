/* Independent small semantic model + the real physical store. No runtime or
 * general family-checkpoint claim. Model state is next sequence and value sum. */
#include "roll_output.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL output:%d: %s\n",__LINE__,#x);exit(1); } } while(0)
#define CAP 24576u
typedef struct { uint64_t next,sum,since_boundary,floor; } model;
typedef struct { unsigned calls,mode,policy,projects,fail_projection; } encoder;
typedef struct { uint8_t bytes[CAP]; uint64_t length; unsigned writes,syncs,fail_write,fail_sync,short_io; } memory;
typedef struct {
    memory *m; pfr_store *store; void *scratch; pfo_output *out;
    pfr_config cfg; pfr_io io; pfo_buffers buffers;
    model arenas[2]; uint8_t cp[3][32],payload[48]; encoder enc;
} fixture;
static void w32(uint8_t *p,uint32_t n){unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void w64(uint8_t *p,uint64_t n){unsigned i;for(i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
static uint64_t r64(const uint8_t *p){uint64_t n=0;unsigned i;for(i=0;i<8;++i)n|=(uint64_t)p[i]<<(8*i);return n;}
static uint32_t crc(const uint8_t *p,size_t n){uint32_t c=UINT32_MAX;unsigned i;while(n--){c^=*p++;for(i=0;i<8;++i)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}return ~c;}
static void chunk(uint8_t p[24],uint64_t seq,unsigned terminal){memset(p,0,24);w32(p,terminal?6:43);w32(p+4,4);w64(p+8,seq);w32(p+16,(uint32_t)(seq+1));w32(p+20,crc(p,20));}
static int64_t rd(void *ctx,uint64_t off,void *p,size_t n){memory *m=ctx;if(off>m->length||n>m->length-off)return -1;memcpy(p,m->bytes+(size_t)off,n);return (int64_t)n;}
static int64_t wr(void *ctx,uint64_t off,const void *p,size_t n){memory *m=ctx;++m->writes;if(m->writes==m->fail_write)return -1;if(off>m->length||n>m->length-off)return -1;if(m->short_io&&n>m->short_io)n=m->short_io;memcpy(m->bytes+(size_t)off,p,n);return (int64_t)n;}
static int resize(void *ctx,uint64_t n){memory *m=ctx;if(n>CAP)return -1;m->length=n;return 0;}
static int size(void *ctx,uint64_t *n){*n=((memory *)ctx)->length;return 0;}
static int sync_io(void *ctx){memory *m=ctx;return ++m->syncs==m->fail_sync?-1:0;}
static int prepare(void *ctx,const void *accepted,void *candidate,const void *raw,size_t n,void *cp,size_t cap,pfo_prepared *result){
    encoder *e=ctx;const model *a=accepted;model *b=candidate;const uint8_t *p=raw;uint8_t *q=cp;
    ++e->calls;CHECK(n==24&&cap==32);CHECK(r64(p+8)==a->next);*b=*a;++b->next;b->sum+=a->next+1;
    if(e->policy)++b->since_boundary;
    if(e->mode==1){result->reason=PFR_SUPPORT;return PFO_RESOURCE;}
    if(e->mode==2){result->checkpoint_bytes=33;return PFR_OK;}
    if(e->mode==3){result->terminal=1;return PFR_OK;}
    if(e->mode==4){return PFR_INVALID;}
    if(e->mode==5){result->reason=99;return PFO_RESOURCE;}
    result->terminal=p[0]==6;
    if(!result->terminal){w64(q,b->next);w64(q+8,b->sum);result->checkpoint_bytes=16;}
    return PFR_OK;
}
static void init(fixture *f){pfr_layout plan;uint8_t initial[16]={0};unsigned i;
    memset(f,0,sizeof *f);f->m=calloc(1,sizeof *f->m);CHECK(f->m);
    f->cfg.max_bytes=CAP;f->cfg.retained_slots=2;f->cfg.checkpoint_bytes=32;f->cfg.payload_bytes=48;f->cfg.session[0]=0x51;
    CHECK(pfr_plan(&f->cfg,&plan)==PFR_OK&&plan.file_bytes==CAP);
    f->store=calloc(1,pfr_state_bytes());f->scratch=malloc(plan.scratch_bytes);f->out=calloc(1,pfo_state_bytes());CHECK(f->store&&f->scratch&&f->out);
    f->io.ctx=f->m;f->io.read_at=rd;f->io.write_at=wr;f->io.resize=resize;f->io.size=size;f->io.sync=sync_io;
    CHECK(pfr_format_v2(f->store,&f->io,&f->cfg,f->scratch,plan.scratch_bytes)==PFR_OK);
    for(i=0;i<2;++i)f->buffers.arenas[i]=&f->arenas[i];
    for(i=0;i<3;++i)f->buffers.checkpoints[i]=f->cp[i];
    f->buffers.arena_bytes=sizeof(model);f->buffers.checkpoint_capacity=32;f->buffers.payload=f->payload;f->buffers.payload_capacity=48;f->buffers.encoder_ctx=&f->enc;f->buffers.prepare=prepare;
    CHECK(pfo_bind(f->out,f->store,&f->buffers,initial,sizeof initial)==PFR_OK);
    f->m->writes=f->m->syncs=0;
}
static void destroy(fixture *f){free(f->out);free(f->scratch);free(f->store);free(f->m);}
static pfo_view view(fixture *f){pfo_view v;CHECK(pfo_get_view(f->out,&v)==PFR_OK);return v;}
static void accept(fixture *f,uint64_t seq,unsigned terminal){uint8_t p[24];chunk(p,seq,terminal);CHECK(pfo_accept(f->out,p,sizeof p)==PFR_OK);}
static void same_accepted(pfo_view a,pfo_view b){CHECK(a.accepted_next_chunk==b.accepted_next_chunk&&a.accepted_payload_bytes==b.accepted_payload_bytes&&a.pending_bytes==b.pending_bytes&&a.accepted_arena==b.accepted_arena);}
static void rotations(void){fixture f;unsigned i; pfo_view v;pfr_view s;init(&f);
    for(i=0;i<20;++i){accept(&f,i,0);v=view(&f);CHECK(v.accepted_next_chunk==i+1&&v.accepted_payload_bytes==24u*(i+1));CHECK(((const model *)v.accepted_arena)->sum==(uint64_t)(i+1)*(i+2)/2);if(i==0)CHECK(v.committed_next_chunk==0&&!f.m->writes);}
    CHECK(pfo_flush(f.out)==PFR_OK);v=view(&f);CHECK(v.accepted_next_chunk==20&&v.committed_next_chunk==20&&v.pending_bytes==0);
    CHECK(pfr_get_view(f.store,&s)==PFR_OK&&s.retained_segments==2&&s.evicted_chunks==16);
    for(i=0;i<2;++i){pfr_segment seg;CHECK(pfr_get_segment(f.store,i,&seg)==PFR_OK);CHECK(seg.checkpoint_bytes==16&&r64(seg.checkpoint)==seg.first_chunk);CHECK(r64(seg.checkpoint+8)==seg.first_chunk*(seg.first_chunk+1)/2);CHECK(seg.payload_bytes==48&&r64(seg.payload+8)==seg.first_chunk);}
    CHECK(pfo_finish(f.out)==PFR_INVALID);accept(&f,20,1);CHECK(pfo_finish(f.out)==PFR_OK);v=view(&f);CHECK(v.phase==PFO_FINISHED&&v.terminal&&v.committed_next_chunk==21);CHECK(pfr_get_view(f.store,&s)==PFR_OK&&s.state==PFR_SEALED);
    i=f.m->writes+f.m->syncs;CHECK(pfo_flush(f.out)==PFR_END&&pfo_finish(f.out)==PFR_END);CHECK(i==f.m->writes+f.m->syncs);destroy(&f);
}
static void refused(void){unsigned mode;for(mode=1;mode<=5;++mode){fixture f;pfo_view a,b;pfr_view s;uint8_t p[24];model old;unsigned calls;init(&f);accept(&f,0,0);accept(&f,1,0);a=view(&f);old=*(const model *)a.accepted_arena;f.enc.mode=mode;calls=f.m->writes+f.m->syncs;chunk(p,2,0);
    CHECK(pfo_accept(f.out,p,24)!=PFR_OK);CHECK(calls==f.m->writes+f.m->syncs);b=view(&f);same_accepted(a,b);CHECK(!memcmp(&old,b.accepted_arena,sizeof old));
    if(mode==1||mode==2){CHECK(b.phase==PFO_STOPPING);CHECK(b.reason==(mode==1?PFR_SUPPORT:PFR_CHECKPOINT_BYTES));CHECK(pfo_accept(f.out,p,24)==PFO_RESOURCE);CHECK(pfo_finish(f.out)==PFR_OK);CHECK(pfr_get_view(f.store,&s)==PFR_OK&&s.state==PFR_STOPPED_RESOURCE&&s.next_chunk==2&&s.bound_payload_bytes==48);}
    else {CHECK(b.phase==PFO_POISONED);CHECK(pfo_flush(f.out)==PFR_FAILED);}
    destroy(&f);}}
static void faults(void){unsigned mode;for(mode=0;mode<4;++mode){fixture f;pfo_view a,b;pfr_view s;uint8_t p[24];model old;unsigned calls;init(&f);accept(&f,0,0);accept(&f,1,0);a=view(&f);old=*(const model *)a.accepted_arena;chunk(p,2,0);
    if(mode<2)f.m->fail_write=mode+1;else f.m->fail_sync=mode-1;
    CHECK(pfo_accept(f.out,p,24)==PFR_IO);b=view(&f);same_accepted(a,b);CHECK(!memcmp(&old,b.accepted_arena,sizeof old)&&b.phase==PFO_POISONED);calls=f.m->writes+f.m->syncs;
    CHECK(pfo_accept(f.out,p,24)==PFR_FAILED&&pfo_flush(f.out)==PFR_FAILED&&pfo_finish(f.out)==PFR_FAILED);CHECK(calls==f.m->writes+f.m->syncs);
    /* Failed root sync may expose either old or new coherent physical commit;
     * the refused candidate itself must never appear. */
    f.m->fail_write=f.m->fail_sync=0;CHECK(pfr_open(f.store,&f.io,&f.cfg,f.scratch,4096)==PFR_OK);CHECK(pfr_get_view(f.store,&s)==PFR_OK);CHECK(s.next_chunk==0||s.next_chunk==2);destroy(&f);}}
static void inputs(void){fixture f;uint8_t p[64],initial[16]={0};pfo_view a;unsigned calls;init(&f);a=view(&f);calls=f.enc.calls;
    chunk(p,1,0);CHECK(pfo_accept(f.out,p,24)==PFR_INVALID);chunk(p,0,0);p[20]^=1;CHECK(pfo_accept(f.out,p,24)==PFR_INVALID);chunk(p,0,0);CHECK(pfo_accept(f.out,p,23)==PFR_INVALID);CHECK(pfo_accept(f.out,p,25)==PFR_INVALID);
    chunk(f.payload,0,0);CHECK(pfo_accept(f.out,f.payload,24)==PFR_INVALID);same_accepted(a,view(&f));CHECK(f.enc.calls==calls&&!f.m->writes);
    f.buffers.checkpoints[2]=f.buffers.checkpoints[0];CHECK(pfo_bind(f.out,f.store,&f.buffers,initial,16)==PFR_INVALID);destroy(&f);
}
static void short_and_partial(void){fixture f;pfo_view v;unsigned i;uint8_t status[24];init(&f);f.m->short_io=17;accept(&f,0,0);CHECK(pfo_flush(f.out)==PFR_OK);v=view(&f);CHECK(v.committed_next_chunk==1&&v.pending_bytes==0);i=f.m->writes+f.m->syncs;CHECK(pfo_flush(f.out)==PFR_OK);CHECK(i==f.m->writes+f.m->syncs);
    /* Wire type7 is not TRAILER. Only the original type6 ends this stream. */
    chunk(status,1,0);w32(status,7);w32(status+20,crc(status,20));CHECK(pfo_accept(f.out,status,24)==PFR_OK);CHECK(!view(&f).terminal);
    accept(&f,2,1);CHECK(pfo_accept(f.out,"",0)==PFR_END);CHECK(pfo_finish(f.out)==PFR_OK);destroy(&f);}
static int support_pressure(void *ctx,const void *candidate){
    encoder *e=ctx;const model *m=candidate;CHECK(e->policy);return m->since_boundary>=2;
}
static int support_project(void *ctx,const void *accepted,void *projected,uint64_t floor,
                           void *checkpoint,size_t cap,size_t *written){
    encoder *e=ctx;const model *a=accepted;model *b=projected;uint8_t *p=checkpoint;
    CHECK(e->policy&&cap==32&&floor>=a->floor&&floor<=a->next);++e->projects;
    if(e->fail_projection)return PFR_INVALID;
    *b=*a;b->since_boundary=0;b->floor=floor;
    w64(p,b->next);w64(p+8,b->sum);w64(p+16,0);w64(p+24,floor);*written=32;return PFR_OK;
}
static void projected_support(void){
    fixture f;unsigned i;init(&f);f.enc.policy=1;
    CHECK(pfo_support(f.out,support_pressure,support_project)==PFR_OK);
    CHECK(pfo_support(f.out,support_pressure,support_project)==PFR_INVALID);
    for(i=0;i<20;++i){pfo_view v;pfr_view store;const model *m;
        accept(&f,i,0);v=view(&f);m=v.accepted_arena;
        CHECK(pfr_get_view(f.store,&store)==PFR_OK);
        CHECK(v.accepted_next_chunk==i+1&&m->next==i+1&&m->sum==(uint64_t)(i+1)*(i+2)/2);
        CHECK(m->since_boundary==1&&m->floor==store.first_chunk);
        CHECK(v.pending_bytes==24&&f.enc.projects==i);
        if(i>2)CHECK(store.first_chunk==i-2);
    }
    CHECK(f.enc.calls==39); /* original preflight plus revalidation after each projection */
    accept(&f,20,1);CHECK(pfo_finish(f.out)==PFR_OK&&view(&f).committed_next_chunk==21);
    CHECK(f.enc.projects==19); /* no successor projection after the actual trailer */
    destroy(&f);
    for(i=0;i<3;++i){uint8_t p[24];pfo_view before;unsigned io;init(&f);f.enc.policy=1;
        CHECK(pfo_support(f.out,support_pressure,support_project)==PFR_OK);accept(&f,0,0);
        before=view(&f);io=f.m->writes+f.m->syncs;chunk(p,1,0);
        if(i==0)f.enc.mode=4;else if(i==1)f.m->fail_sync=1;else f.enc.fail_projection=1;
        CHECK(pfo_accept(f.out,p,sizeof p)!=PFR_OK);
        CHECK(view(&f).accepted_next_chunk==before.accepted_next_chunk);
        CHECK(((const model *)view(&f).accepted_arena)->next==1);
        if(i==0)CHECK(f.m->writes+f.m->syncs==io&&!f.enc.projects);
        if(i==1)CHECK(!f.enc.projects);
        if(i==2)CHECK(f.enc.projects==1&&view(&f).committed_next_chunk==1);
        destroy(&f);
    }
}
int main(int argc,char **argv){
    if(argc==1){rotations();refused();faults();inputs();short_and_partial();projected_support();}
    else if(argc!=2)return 2;
    else if(!strcmp(argv[1],"rotations"))rotations();
    else if(!strcmp(argv[1],"refused"))refused();
    else if(!strcmp(argv[1],"faults"))faults();
    else if(!strcmp(argv[1],"inputs"))inputs();
    else if(!strcmp(argv[1],"short"))short_and_partial();
    else if(!strcmp(argv[1],"projection"))projected_support();
    else return 2;
    puts("PASS output ownership, rotations, refusals and fault boundaries");return 0;
}
