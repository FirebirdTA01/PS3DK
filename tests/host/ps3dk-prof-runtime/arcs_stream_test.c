/* Caller-storage and wire-oracle tests. The wrapper is fixture code, not the
 * future file exporter. Near-MAX fields are seeded only to test arithmetic. */
#include "prof_arcs_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static unsigned char body[1048576], reference[1048576];
static size_t used, ref_used;
static unsigned calls, fail_call, partial, reenter;
static struct ps3dk_prof_arc_stream stream;
static struct ps3dk_prof_stream_slot slots[2];
static struct ps3dk_prof_stream_final finals[2];
static struct ps3prof_arc_row rows[8];
static int put(void *ctx,const void *p,size_t n) {
    (void)ctx; ++calls;
    CHECK(n<=sizeof body-used);
    if(reenter) {
        CHECK(ps3dk_prof__stream_finish(&stream,0)==PS3DK_PROF_STREAM_STATE);
        CHECK(ps3dk_prof__stream_accept(&stream,NULL)==PS3DK_PROF_STREAM_STATE);
    }
    if(calls==fail_call) {if(partial){memcpy(body+used,p,n/2);used+=n/2;}return -1;}
    memcpy(body+used,p,n);used+=n;return 0;
}
static int ref_put(void *ctx,const void *p,size_t n) {
    (void)ctx; CHECK(n<=sizeof reference-ref_used);
    memcpy(reference+ref_used,p,n);ref_used+=n;return 0;
}
static void init(uint64_t budget) {
    memset(&stream,0,sizeof stream);memset(slots,0,sizeof slots);memset(finals,0,sizeof finals);
    used=0;calls=0;fail_call=0;partial=0;reenter=0;
    CHECK(ps3dk_prof__stream_init(&stream,slots,finals,2,8,budget,put,NULL)==0);
}
static struct ps3dk_prof_arc_view view(unsigned inc,unsigned slot,unsigned state,unsigned row) {
    rows[row]=(struct ps3prof_arc_row){.caller_return_pc=4,.callee_hook_pc=8,.count=inc,.flags=0};
    struct ps3dk_prof_arc_view v={.generation=inc,.thread={
        .incarnation=inc,.slot=slot,.os_thread_id=42+slot,.name="worker",
        .rows=&rows[row],.row_count=1,.state=(uint8_t)state,.attempted=inc,.recorded=inc}};
    if(state==3) {
        v.thread.rows=NULL;v.thread.row_count=0;v.thread.limit_flags=2;
        v.thread.attempted=v.thread.recorded=v.thread.refused=v.thread.lost=UINT64_MAX;
    }
    return v;
}
static unsigned u32(const unsigned char *p) {
    return (unsigned)p[0]|(unsigned)p[1]<<8|(unsigned)p[2]<<16|(unsigned)p[3]<<24;
}
static uint64_t u64(const unsigned char *p) { return u32(p)|((uint64_t)u32(p+4)<<32); }
static void wrap(const char *out,const char *name,struct ps3dk_prof_arc_view *vs,unsigned count,int mode) {
    struct ps3prof_arc_thread ts[8];
    CHECK(count<=8);
    for(unsigned i=0;i<count;++i) ts[i]=vs[i].thread;
    struct ps3prof_arc_snapshot s={.app="stream",.build_id=(const unsigned char *)"build",.build_id_len=5,
        .sdk_version="test",.compiler_identity="host",.flags="",.workload_tag="",
        .abi=1,.mode=1,.snapshot_flags=1,.session_id=1,.snapshot_id=1,.timing_chunk_seq=UINT64_MAX,
        .max_threads=2,.max_arcs_per_thread=8,.freq_hz=1000,.threads=ts,.thread_count=count};
    ref_used=0;CHECK(ps3prof_arcs_write(&s,ref_put,NULL)==0);
    size_t prefix=16;
    for(unsigned i=0;i<2;++i) prefix+=20+u32(reference+prefix+4);
    size_t trailer=prefix;
    while(u32(reference+trailer)!=6) trailer+=20+u32(reference+trailer+4);
    if(mode==0) {
        CHECK(used==trailer-prefix);CHECK(memcmp(body,reference+prefix,used)==0);
        unsigned active=0,flags=0;
        for(unsigned i=0;i<count;++i) {
            const struct ps3prof_arc_thread *t=&ts[i];
            if(t->state==3)flags|=1;
            if(t->limit_flags&1)flags|=2;
            if(t->state==1)continue;
            const struct ps3dk_prof_stream_final *f=&finals[active++];
            CHECK(f->incarnation==t->incarnation && f->state==t->state && f->limit_flags==t->limit_flags);
            CHECK(f->attempted==t->attempted && f->recorded==t->recorded && f->refused==t->refused && f->lost==t->lost);
        }
        CHECK(stream.final_count==active && stream.end_flags==flags);
        CHECK(stream.body_bytes==used && stream.next_sequence==u64(reference+trailer+8));
    }
    char path[1024];int n=snprintf(path,sizeof path,"%s/%s.arcs",out,name);
    CHECK(n>0 && (size_t)n<sizeof path);FILE *f=fopen(path,"wb");CHECK(f!=NULL);
    CHECK(fwrite(reference,1,prefix,f)==prefix);CHECK(fwrite(body,1,used,f)==used);
    if(mode!=1) CHECK(fwrite(reference+trailer,1,ref_used-trailer,f)==ref_used-trailer);
    CHECK(fclose(f)==0);
}
static void invalid(struct ps3dk_prof_arc_view v) {
    init(10000);CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_INVALID);
    CHECK(calls==0 && used==0);CHECK(ps3dk_prof__stream_finish(&stream,0)!=0);
    CHECK(ps3dk_prof__stream_accept(&stream,&v)!=0);CHECK(calls==0);
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;
    const char *which=argv[1],*out=argv[2];
    if(!strcmp(which,"config")) {
        CHECK(ps3dk_prof__stream_init(NULL,slots,finals,2,8,100,put,NULL)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_init(&stream,slots,finals,0,8,100,put,NULL)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_init(&stream,slots,finals,1025,8,100,put,NULL)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_init(&stream,slots,finals,2,0,100,put,NULL)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_init(&stream,slots,finals,2,65537,100,put,NULL)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_init(&stream,NULL,finals,2,8,100,put,NULL)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_init(&stream,slots,NULL,2,8,100,put,NULL)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_init(&stream,slots,finals,2,8,100,NULL,NULL)==PS3DK_PROF_STREAM_INVALID);
        init(0);struct ps3dk_prof_arc_stream before=stream;
        CHECK(ps3dk_prof__stream_init(&stream,slots,finals,2,8,100,put,NULL)==PS3DK_PROF_STREAM_STATE);
        CHECK(memcmp(&stream,&before,sizeof stream)==0);
        CHECK(ps3dk_prof__stream_finish(&stream,7)==0);CHECK(stream.unregistered_refusals==7 && calls==0);
        CHECK(ps3dk_prof__stream_finish(&stream,8)==PS3DK_PROF_STREAM_STATE);
        CHECK(stream.unregistered_refusals==7);
        wrap(out,"empty",NULL,0,0);
    } else if(!strcmp(which,"reuse")) {
        init(10000);reenter=1;struct ps3dk_prof_arc_view vs[4];
        for(unsigned i=0;i<4;++i){vs[i]=view(i+1,0,1,i);CHECK(ps3dk_prof__stream_accept(&stream,&vs[i])==0);}
        CHECK(ps3dk_prof__stream_finish(&stream,3)==0);CHECK(stream.unregistered_refusals==3);
        CHECK(ps3dk_prof__stream_accept(&stream,&vs[0])==PS3DK_PROF_STREAM_STATE);
        wrap(out,"reuse",vs,4,0);
    } else if(!strcmp(which,"mixed")) {
        init(10000);struct ps3dk_prof_arc_view vs[4]={view(2,1,1,0),view(1,0,1,1),view(3,0,2,2),view(4,1,3,3)};
        for(unsigned i=0;i<4;++i)CHECK(ps3dk_prof__stream_accept(&stream,&vs[i])==0);
        CHECK(ps3dk_prof__stream_finish(&stream,0)==0);wrap(out,"mixed",vs,4,0);
    } else if(!strcmp(which,"duplicate")) {
        init(10000);struct ps3dk_prof_arc_view vs[2]={view(1,0,1,0),view(1,1,1,1)};
        CHECK(ps3dk_prof__stream_accept(&stream,&vs[0])==0);CHECK(ps3dk_prof__stream_accept(&stream,&vs[1])==0);
        CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
        vs[1].thread.incarnation=2; /* valid reference envelope/trailer; actual body stays duplicate */
        wrap(out,"duplicate",vs,2,2);
    } else if(!strcmp(which,"duplicate-key")) {
        init(10000);struct ps3dk_prof_arc_view v=view(1,0,1,0);
        rows[1]=rows[0];v.thread.row_count=2;v.thread.recorded=v.thread.attempted=2;
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
        rows[1].callee_hook_pc=12; /* valid reference, actual encoded keys remain duplicate */
        wrap(out,"duplicate-key",&v,1,2);
    } else if(!strcmp(which,"names")) {
        init(10000);struct ps3dk_prof_arc_view v=view(1,0,2,0);char name[514];
        memset(name,'a',512);name[512]=0;v.thread.name=name;
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
        wrap(out,"name512",&v,1,0);
        name[512]='a';name[513]=0;invalid(v);
        v.thread.name="\xf0\x9f\x8e\xae";init(10000);
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
        wrap(out,"utf8",&v,1,0);
        v.thread.name="\xc0\x80";invalid(v);v.thread.name="\xed\xa0\x80";invalid(v);
    } else if(!strcmp(which,"lifetime")) {
        init(10000);struct ps3dk_prof_arc_view v=view(1,0,2,0);char name[7]="worker";
        v.thread.name=name;CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);
        struct ps3dk_prof_stream_final saved=finals[0];
        memset(name,'x',6);memset(rows,0,sizeof rows);memset(&v,0,sizeof v);
        CHECK(memcmp(&saved,&finals[0],sizeof saved)==0);
        CHECK(ps3dk_prof__stream_finish(&stream,0)==0);v=view(1,0,2,0);wrap(out,"lifetime",&v,1,0);
    } else if(!strcmp(which,"budget")) {
        struct ps3dk_prof_arc_view v=view(1,0,1,0);
        const uint64_t cost=48+60+64; /* THREADS:42+strlen(worker), one ARCS, RETIRED */
        init(cost);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);CHECK(used==cost);
        CHECK(ps3dk_prof__stream_finish(&stream,0)==0);wrap(out,"budget",&v,1,0);
        init(cost-1);CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_LIMIT);CHECK(calls==0);
        CHECK(ps3dk_prof__stream_finish(&stream,0)==PS3DK_PROF_STREAM_LIMIT);
        init(UINT64_MAX);stream.body_bytes=UINT64_MAX-1;
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_LIMIT);CHECK(calls==0);
        init(UINT64_MAX);stream.next_sequence=UINT64_MAX-3;
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_LIMIT);CHECK(calls==0);
        init(UINT64_MAX);stream.next_sequence=UINT64_MAX-4;
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);CHECK(stream.next_sequence==UINT64_MAX-1);
    } else if(!strcmp(which,"failures")) {
        struct ps3dk_prof_arc_view v=view(1,0,1,0);
        for(unsigned i=1;i<=3;++i)for(unsigned p=0;p<2;++p){
            init(10000);fail_call=i;partial=p;
            CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_IO);unsigned old_calls=calls;
            CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_IO);
            CHECK(ps3dk_prof__stream_finish(&stream,0)==PS3DK_PROF_STREAM_IO);CHECK(calls==old_calls);
            char name[32];snprintf(name,sizeof name,"failure-%u-%u",i,p);wrap(out,name,&v,1,1);
        }
    } else if(!strcmp(which,"validation")) {
        struct ps3dk_prof_arc_view v;
#define BAD(expr) do {v=view(1,0,2,0);expr;invalid(v);} while(0)
        BAD(v.generation=0);BAD(v.thread.incarnation=0);BAD(v.thread.slot=2);BAD(v.thread.state=0);
        BAD(v.thread.rows=NULL);BAD(v.thread.row_count=9);BAD(v.thread.name=NULL);BAD(v.thread.name="\xff");
        BAD(v.thread.attempted=0);BAD(v.thread.recorded=0);BAD(v.thread.refused=UINT64_MAX);
        BAD(v.thread.limit_flags=2);BAD(rows[0].count=0);BAD(rows[0].callee_hook_pc=3);
        BAD(rows[0].caller_return_pc=0);BAD(rows[0].flags=2);BAD(rows[0].flags=1);
        v=view(1,0,3,0);v.thread.recorded=0;invalid(v);
        v=view(1,0,3,0);v.thread.row_count=1;v.thread.rows=rows;invalid(v);
    } else if(!strcmp(which,"transitions")) {
        struct ps3dk_prof_arc_view v=view(1,0,2,0);
        init(10000);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);size_t before=used;
        v=view(2,0,1,1);CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_INVALID);CHECK(used==before);
        v=view(1,0,1,0);init(10000);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);before=used;
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_INVALID);CHECK(used==before);
        v=view(1,0,1,0);v.generation=UINT32_MAX;init(10000);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);
        before=used;v=view(2,0,1,1);CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_INVALID);CHECK(used==before);
    } else if(!strcmp(which,"limits")) {
        struct ps3dk_prof_arc_view v=view(1,0,2,0);
        rows[0].count=v.thread.attempted=v.thread.recorded=UINT64_MAX;
        init(10000);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
        wrap(out,"max-exact",&v,1,0);
        v.thread.limit_flags=1;init(10000);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);
        CHECK(ps3dk_prof__stream_finish(&stream,0)==0);wrap(out,"limit",&v,1,0);
        v=view(1,0,2,0);v.thread.attempted=3;v.thread.refused=1;v.thread.lost=1;
        init(10000);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
        wrap(out,"refused",&v,1,0);
    } else if(!strcmp(which,"long")) {
        init(sizeof body);struct ps3dk_prof_stream_slot *sp=stream.slots;
        struct ps3dk_prof_stream_final *fp=stream.finals;
        for(unsigned i=1;i<=1000;++i){struct ps3dk_prof_arc_view v=view(i,0,1,0);CHECK(ps3dk_prof__stream_accept(&stream,&v)==0);}
        CHECK(stream.slots==sp && stream.finals==fp && stream.final_count==0);
        CHECK(used==172000 && stream.next_sequence==3002);CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
    } else return 2;
    printf("stream checks=%u pass=%u fail=0\n",checks,checks);return 0;
}
