/* Physical terminal declarations only: no semantic completion claim. */
#include "roll_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE 4096u
#define PREFIX 12288u
#define BYTES 28672u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL terminal:%d: %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct {
    uint8_t data[BYTES]; uint64_t length;
    unsigned writes, syncs, fail_write, fail_sync, short_limit, bad_return;
    size_t tear;
} memory;
typedef struct { memory *m; pfr_store *s; void *scratch; pfr_io io; pfr_config cfg; } fixture;
static uint64_t get64(const uint8_t *p) { uint64_t n=0; unsigned i; for(i=0;i<8;++i)n|=(uint64_t)p[i]<<(8*i);return n; }
static void put32(uint8_t *p,uint32_t n) { unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i)); }
static void put64(uint8_t *p,uint64_t n) { unsigned i;for(i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i)); }
static uint32_t crc(const uint8_t *p,size_t n) {
    uint32_t c=UINT32_MAX;unsigned j;
    while(n--){c^=*p++;for(j=0;j<8;++j)c=(c>>1)^(UINT32_C(0xedb88320)&(0u-(c&1u)));}return ~c;
}
static void root_crc(uint8_t *r) { put32(r+PAGE-4,crc(r,PAGE-4)); }
static uint8_t *root_at(memory *m,uint64_t rev) { return m->data+PAGE+(size_t)(rev&1u)*PAGE; }
static int64_t rd(void *ctx,uint64_t at,void *p,size_t n) {
    memory *m=ctx;if(at>m->length || n>m->length-at)return -1;memcpy(p,m->data+(size_t)at,n);return (int64_t)n;
}
static int64_t wr(void *ctx,uint64_t at,const void *p,size_t n) {
    memory *m=ctx;size_t take=n;++m->writes;
    if(at>m->length || n>m->length-at)return -1;
    if(m->bad_return==1)return 0;
    if(m->bad_return==2)return (int64_t)n+1;
    if(m->fail_write==m->writes){take=m->tear<n?m->tear:n;memcpy(m->data+(size_t)at,p,take);return -1;}
    if(m->short_limit && take>m->short_limit)take=m->short_limit;
    memcpy(m->data+(size_t)at,p,take);return (int64_t)take;
}
static int resize_file(void *ctx,uint64_t n){memory *m=ctx;if(n>BYTES)return -1;m->length=n;return 0;}
static int size_file(void *ctx,uint64_t *n){*n=((memory *)ctx)->length;return 0;}
static int sync_file(void *ctx){memory *m=ctx;++m->syncs;return m->fail_sync==m->syncs?-1:0;}
static void attach(fixture *f,memory *m) {
    pfr_layout layout;memset(f,0,sizeof *f);f->m=m;
    f->cfg.max_bytes=BYTES;f->cfg.retained_slots=3;f->cfg.checkpoint_bytes=128;f->cfg.payload_bytes=256;f->cfg.session[0]=0x42;
    CHECK(pfr_plan(&f->cfg,&layout)==PFR_OK);CHECK(layout.file_bytes==BYTES);
    f->s=malloc(pfr_state_bytes());f->scratch=malloc(layout.scratch_bytes);CHECK(f->s && f->scratch);
    f->io.ctx=m;f->io.read_at=rd;f->io.write_at=wr;f->io.resize=resize_file;f->io.size=size_file;f->io.sync=sync_file;
}
static void destroy(fixture *f){free(f->scratch);free(f->s);}
static pfr_view view(fixture *f){pfr_view v;CHECK(pfr_get_view(f->s,&v)==PFR_OK);return v;}
static pfr_transition request(fixture *f,uint32_t state,uint32_t reason) {
    pfr_view v=view(f);pfr_transition t;
    t.expected_revision=v.generation;t.next_chunk=v.next_chunk;t.payload_bytes=v.bound_payload_bytes;t.state=state;t.reason=reason;return t;
}
static void chunk(fixture *f) {
    uint8_t cp[8],p[24];pfr_view v=view(f);memset(p,0,sizeof p);put64(cp,v.next_segment);
    put32(p,43);put32(p+4,4);put64(p+8,v.next_chunk);put32(p+20,crc(p,20));
    CHECK(pfr_commit(f->s,cp,sizeof cp,p,sizeof p)==PFR_OK);
}
static void save(const memory *m,const char *dir,const char *name) {
    char path[4096];FILE *f;int n;if(!dir)return;n=snprintf(path,sizeof path,"%s/%s",dir,name);CHECK(n>0 && (size_t)n<sizeof path);
    f=fopen(path,"wb");CHECK(f);CHECK(fwrite(m->data,1,(size_t)m->length,f)==m->length);CHECK(fclose(f)==0);
}
static void open_existing(fixture *f,memory *m){attach(f,m);CHECK(pfr_open(f->s,&f->io,&f->cfg,f->scratch,PAGE)==PFR_OK);}
static void reset_calls(memory *m){m->writes=m->syncs=m->fail_write=m->fail_sync=m->short_limit=m->bad_return=0;m->tear=0;}
static void same_chain(pfr_view a,pfr_view b) {
    CHECK(a.first_segment==b.first_segment && a.next_segment==b.next_segment);
    CHECK(a.first_chunk==b.first_chunk && a.next_chunk==b.next_chunk);
    CHECK(a.evicted_segments==b.evicted_segments && a.evicted_chunks==b.evicted_chunks);
    CHECK(a.evicted_payload_bytes==b.evicted_payload_bytes && a.bound_payload_bytes==b.bound_payload_bytes);
    CHECK(a.retained_segments==b.retained_segments);
}
static void publish(fixture *f,uint32_t state,uint32_t reason) {
    uint8_t before[BYTES];pfr_view a=view(f),b;pfr_transition t=request(f,state,reason);size_t i;
    memcpy(before,f->m->data,BYTES);reset_calls(f->m);
    CHECK(pfr_publish_state(f->s,&t)==PFR_OK);b=view(f);same_chain(a,b);
    CHECK(b.generation==a.generation+1 && b.state==state && b.reason==reason && b.format_version==2);
    CHECK(f->m->writes==1 && f->m->syncs==1 && f->m->length==BYTES);
    for(i=0;i<BYTES;++i) if(i<PAGE+(size_t)(b.generation&1u)*PAGE || i>=PAGE+(size_t)(b.generation&1u)*PAGE+PAGE) CHECK(before[i]==f->m->data[i]);
}
static void make_base(memory *m) {
    fixture f;unsigned i;memset(m,0,sizeof *m);attach(&f,m);
    CHECK(pfr_format_v2(f.s,&f.io,&f.cfg,f.scratch,PAGE)==PFR_OK); /* actual RED: unavailable behavior */
    CHECK(view(&f).state==PFR_OPEN);
    for(i=0;i<12;++i) { chunk(&f); }
    publish(&f,PFR_OPEN,0);publish(&f,PFR_OPEN,0);chunk(&f);
    CHECK(view(&f).generation==16 && view(&f).next_chunk==13 && view(&f).bound_payload_bytes==312);
    destroy(&f);reset_calls(m);
}
static void terminal_states(const memory *base,memory *work,const char *dir) {
    fixture f;unsigned reason;save(base,dir,"open.profr");
    for(reason=0;reason<=5;++reason) {
        pfr_transition t;pfr_view v;unsigned calls;char name[32];
        memcpy(work,base,sizeof *work);open_existing(&f,work);publish(&f,reason?PFR_STOPPED_RESOURCE:PFR_SEALED,reason);v=view(&f);
        CHECK(v.next_segment==13 && v.first_segment==10);t=request(&f,v.state,reason);calls=work->writes+work->syncs;
        CHECK(pfr_publish_state(f.s,&t)==PFR_END);CHECK(pfr_commit(f.s,NULL,0,NULL,0)==PFR_END);
        CHECK(calls==work->writes+work->syncs);
        if(reason) (void)snprintf(name,sizeof name,"stopped-%u.profr",reason);else (void)snprintf(name,sizeof name,"sealed.profr");
        save(work,dir,name);destroy(&f);
        if(!reason){root_at(work,17)[120]^=1;open_existing(&f,work);v=view(&f);CHECK(v.recovered && v.state==PFR_OPEN && v.generation==16);destroy(&f);save(work,dir,"recovered.profr");}
    }
}
static void invalid_requests(const memory *base,memory *work) {
    fixture f;unsigned mode;
    for(mode=0;mode<8;++mode){pfr_transition t;memcpy(work,base,sizeof *work);open_existing(&f,work);t=request(&f,PFR_SEALED,0);reset_calls(work);
        switch(mode){case 0:t.state=0;break;case 1:t.state=4;break;case 2:t.reason=1;break;case 3:t.state=PFR_STOPPED_RESOURCE;break;
        case 4:t.state=PFR_STOPPED_RESOURCE;t.reason=6;break;case 5:--t.expected_revision;break;case 6:++t.next_chunk;break;default:++t.payload_bytes;break;}
        CHECK(pfr_publish_state(f.s,&t)==PFR_INVALID);CHECK(!work->writes && !work->syncs);CHECK(!memcmp(work->data,base->data,BYTES));destroy(&f);
    }
    memcpy(work,base,sizeof *work);attach(&f,work);f.io.write_at=NULL;f.io.sync=NULL;CHECK(pfr_open(f.s,&f.io,&f.cfg,f.scratch,PAGE)==PFR_OK);
    {pfr_transition t=request(&f,PFR_SEALED,0);CHECK(pfr_publish_state(f.s,&t)==PFR_INVALID);}destroy(&f);
}
static void faults(const memory *base,memory *work) {
    fixture f;size_t cut;unsigned mode;
    /* Every possible torn prefix, including a complete root followed by error. */
    for(cut=0;cut<=PAGE;++cut){pfr_transition t;pfr_view v;unsigned calls;memcpy(work,base,sizeof *work);open_existing(&f,work);t=request(&f,PFR_STOPPED_RESOURCE,PFR_SUPPORT);reset_calls(work);work->fail_write=1;work->tear=cut;
        CHECK(pfr_publish_state(f.s,&t)==PFR_IO);calls=work->writes+work->syncs;
        CHECK(pfr_publish_state(f.s,&t)==PFR_FAILED);CHECK(pfr_commit(f.s,NULL,0,NULL,0)==PFR_FAILED);CHECK(calls==work->writes+work->syncs);
        CHECK(!memcmp(work->data+PREFIX,base->data+PREFIX,BYTES-PREFIX));destroy(&f);reset_calls(work);open_existing(&f,work);v=view(&f);
        CHECK((v.generation==16 && v.state==PFR_OPEN)||(v.generation==17 && v.state==PFR_STOPPED_RESOURCE));destroy(&f);
    }
    for(mode=0;mode<4;++mode){pfr_transition t;memcpy(work,base,sizeof *work);open_existing(&f,work);t=request(&f,PFR_SEALED,0);reset_calls(work);
        if(mode==0)work->fail_sync=1;else if(mode<3)work->bad_return=mode;else work->short_limit=31;
        CHECK(pfr_publish_state(f.s,&t)==(mode==3?PFR_OK:PFR_IO));
        if(mode<3){unsigned calls=work->writes+work->syncs;CHECK(pfr_publish_state(f.s,&t)==PFR_FAILED);CHECK(calls==work->writes+work->syncs);}destroy(&f);
    }
}
static void pair_refusals(const memory *base,memory *work) {
    fixture f;unsigned mode;
    for(mode=0;mode<4;++mode){uint8_t *old,*newer;memcpy(work,base,sizeof *work);open_existing(&f,work);publish(&f,PFR_OPEN,0);destroy(&f);
        old=root_at(work,16);newer=root_at(work,17);
        if(mode==0)put64(newer+8,19);
        else if(mode==1)put32(old+96,PFR_SEALED);
        else if(mode==2){put64(newer+72,241);put64(newer+112,313);}
        else { /* Empty prior OPEN is individually valid, but skips 13 segments. */
            memset(old,0,PAGE);memcpy(old,"PFRROOT2",8);put64(old+8,16);old[80]=0x42;put32(old+96,PFR_OPEN);
        }
        root_crc(old);root_crc(newer);attach(&f,work);CHECK(pfr_open(f.s,&f.io,&f.cfg,f.scratch,PAGE)==PFR_CORRUPT);destroy(&f);
    }
}
static void empty_and_legacy(memory *work) {
    fixture f;pfr_transition t;pfr_view v;
    memset(work,0,sizeof *work);attach(&f,work);CHECK(pfr_format_v2(f.s,&f.io,&f.cfg,f.scratch,PAGE)==PFR_OK);publish(&f,PFR_STOPPED_RESOURCE,PFR_CHECKPOINT_BYTES);v=view(&f);CHECK(!v.retained_segments && !v.next_chunk);destroy(&f);
    memset(work,0,sizeof *work);attach(&f,work);CHECK(pfr_format(f.s,&f.io,&f.cfg,f.scratch,PAGE)==PFR_OK);v=view(&f);CHECK(v.format_version==1 && v.state==PFR_LEGACY_UNSPECIFIED);
    memset(&t,0,sizeof t);t.expected_revision=1;t.state=PFR_SEALED;reset_calls(work);CHECK(pfr_publish_state(f.s,&t)==PFR_INVALID);CHECK(!work->writes && !work->syncs);destroy(&f);
}
static void max_revision(memory *work) {
    fixture f;pfr_transition t;uint8_t *r;
    memset(work,0,sizeof *work);attach(&f,work);CHECK(pfr_format_v2(f.s,&f.io,&f.cfg,f.scratch,PAGE)==PFR_OK);destroy(&f);
    r=root_at(work,UINT64_MAX);put64(r+8,UINT64_MAX);root_crc(r);
    memcpy(root_at(work,UINT64_MAX-1),r,PAGE);put64(root_at(work,UINT64_MAX-1)+8,UINT64_MAX-1);root_crc(root_at(work,UINT64_MAX-1));
    open_existing(&f,work);t=request(&f,PFR_SEALED,0);reset_calls(work);CHECK(pfr_publish_state(f.s,&t)==PFR_SPACE);CHECK(pfr_commit(f.s,NULL,0,NULL,0)==PFR_SPACE);CHECK(!work->writes && !work->syncs);destroy(&f);
    CHECK(get64(r+8)==UINT64_MAX);
}
static void large_prefix(const memory *base,memory *work,uint64_t evicted,uint64_t bound) {
    uint8_t saved[PAGE],*r;uint64_t first=UINT64_MAX/256+100;unsigned i;
    memcpy(work,base,sizeof *work);memcpy(saved,root_at(work,16),PAGE);
    memset(work->data+PAGE,0,2*PAGE);r=root_at(work,UINT64_MAX-3);memcpy(r,saved,PAGE);
    put64(r+8,UINT64_MAX-3);put64(r+24,first);put64(r+32,first+3);put64(r+56,first);put64(r+72,evicted);put64(r+112,bound);
    for(i=0;i<3;++i) {
        uint8_t *e=r+128+i*40;uint32_t physical=(uint32_t)e[0];uint8_t *s=work->data+PREFIX+physical*PAGE,*foot=s+PAGE-64;
        put64(e+16,first+i);put64(s+24,first+i);put64(s+32,first+i+2);put32(s+80,crc(s,80));
        put64(foot+8,first+i);put64(foot+16,first+i+2);put32(foot+48,crc(s,PAGE-64));put32(foot+60,crc(foot,60));
    }
    root_crc(r);reset_calls(work);
}
static void byte_overflow(const memory *base,memory *work) {
    fixture f;uint8_t payload[24];
    large_prefix(base,work,UINT64_MAX-50,21);attach(&f,work);
    CHECK(pfr_open(f.s,&f.io,&f.cfg,f.scratch,PAGE)==PFR_CORRUPT);destroy(&f);
    /* Valid predecessor: adding a further 24-byte chunk would overflow the
     * all-history byte binding even though the eviction increment still fits. */
    large_prefix(base,work,UINT64_MAX-80,UINT64_MAX-8);open_existing(&f,work);
    memset(payload,0,sizeof payload);put32(payload,43);put32(payload+4,4);put64(payload+8,13);put32(payload+20,crc(payload,20));
    CHECK(pfr_commit(f.s,NULL,0,payload,sizeof payload)==PFR_SPACE);CHECK(!work->writes && !work->syncs);destroy(&f);
}
int main(int argc,char **argv) {
    memory *base=malloc(sizeof *base),*work=malloc(sizeof *work);CHECK(base && work);
    make_base(base);terminal_states(base,work,argc>1?argv[1]:NULL);invalid_requests(base,work);faults(base,work);pair_refusals(base,work);empty_and_legacy(work);max_revision(work);byte_overflow(base,work);
    free(work);free(base);puts("PASS terminal root transactions (physical declarations only)");return 0;
}
