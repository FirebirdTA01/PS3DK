#define _POSIX_C_SOURCE 200809L
#include "prof_internal.h"
#include "prof_arcs_export.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { __atomic_fetch_add(&checks,1,__ATOMIC_RELAXED); if (!(x)) {fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);} } while (0)
static unsigned char timing[1048576],body[1048576],output[1048576],scratch[31];
static size_t ntiming,nbody,noutput,cursor,available;
static struct ps3dk_prof_arc_stream stream;
static struct ps3dk_prof_stream_slot slots[1];
static struct ps3dk_prof_stream_final finals[1];
static unsigned reads,writes,flushes,closes,eof,reenter;
static const char *mode;
static int is(const char *s) { return !strcmp(mode,s); }
static int append(unsigned char *dst,size_t *used,const void *data,size_t n) {
    CHECK(n<=1048576-*used);memcpy(dst+*used,data,n);*used+=n;return 0;
}
static long timing_write(void *ctx,const void *data,unsigned long n) {
    (void)ctx;if(is("timing-write"))return -1;
    append(timing,&ntiming,data,n);return (long)n;
}
static int timing_flush(void *ctx) { (void)ctx;return is("timing-flush")?-1:0; }
static int timing_close(void *ctx) { (void)ctx;return is("timing-close")?-1:0; }
static int spool(void *ctx,const void *data,size_t n) {(void)ctx;return append(body,&nbody,data,n);}
static int consume(void *ctx,const struct ps3dk_prof_arc_view *v) {(void)ctx;return ps3dk_prof__stream_accept(&stream,v);}
static int finish(void *ctx,uint64_t n) {(void)ctx;return ps3dk_prof__stream_finish(&stream,n);}
static ptrdiff_t read_body(void *,void *,size_t);
static int write_output(void *,const void *,size_t);
static int flush_output(void *);
static int close_output(void *);
static const struct ps3dk_prof_arc_output ops={write_output,flush_output,close_output,NULL};
static int export(void) {return ps3dk_prof__stream_export(&stream,1,read_body,NULL,&ops,scratch,sizeof scratch);}
static void recursive(void) {
    if(!reenter)return;
    unsigned r=reads,w=writes,f=flushes,c=closes;
    CHECK(export()==PS3DK_PROF_STREAM_STATE);
    CHECK(ps3dk_prof__stream_replay(&stream,read_body,NULL,write_output,NULL,scratch,sizeof scratch)==PS3DK_PROF_STREAM_STATE);
    CHECK(reads==r&&writes==w&&flushes==f&&closes==c);
}
static ptrdiff_t read_body(void *ctx,void *buf,size_t n) {
    (void)ctx;++reads;recursive();
    size_t count=available-cursor;if(count>n)count=n;if(is("short")&&count>1)count=1;
    memcpy(buf,body+cursor,count);cursor+=count;if(!count)eof=1;return (ptrdiff_t)count;
}
static int write_output(void *ctx,const void *data,size_t n) {
    (void)ctx;++writes;recursive();
    if((is("output-header")&&!reads)||(is("output-body")&&reads&&!eof)||(is("output-trailer")&&eof)) {
        append(output,&noutput,data,n/2);return -1;
    }
    return append(output,&noutput,data,n);
}
static int flush_output(void *ctx) {(void)ctx;++flushes;recursive();return is("output-flush")?-1:0;}
static int close_output(void *ctx) {(void)ctx;++closes;recursive();return is("output-close")?-1:0;}
static void *worker(void *arg) {
    int rc=ps3dk_prof_thread_begin("worker");
    if(arg) {CHECK(rc==PS3DK_PROF_EFULL);return NULL;}
    CHECK(rc==0);CHECK(ps3dk_prof__record_arc(4,8,0)==0);CHECK(ps3dk_prof__record_arc(4,8,0)==0);
    if(!is("unknown"))ps3dk_prof_thread_end();
    return NULL;
}
static void save(const char *dir,const char *suffix,const void *data,size_t n) {
    char path[1024];int count=snprintf(path,sizeof path,"%s/%s.%s",dir,mode,suffix);
    CHECK(count>0&&(size_t)count<sizeof path);FILE *f=fopen(path,"wb");CHECK(f!=NULL);
    CHECK(fwrite(data,1,n,f)==n);CHECK(fclose(f)==0);
}
int main(int argc,char **argv) {
    if(argc!=3)return 2;mode=argv[1];
    CHECK(ps3dk_prof__stream_init(&stream,slots,finals,1,4,sizeof body,spool,NULL)==0);
    if(is("no-runtime")) {
        CHECK(ps3dk_prof__stream_finish(&stream,0)==0);
        CHECK(export()==PS3DK_PROF_STREAM_TIMING);
        CHECK(!stream.export_attempted&&!reads&&!writes&&!flushes&&!closes);
        printf("checks=%u\n",checks);return 0;
    }
    ps3dk_prof_config cfg;ps3dk_prof_config_defaults(&cfg);
    cfg.max_threads=1;cfg.max_descriptors=8;cfg.ring_records=512;cfg.quiesce_timeout_us=0;
    cfg.app="coordinator";cfg.build_id="build";cfg.flags="flags";cfg.workload_tag="test";
    if(is("thread")||is("join"))cfg.mode=PS3DK_PROF_MODE_THREAD;
    ps3dk_prof_sink_callbacks cb={timing_write,timing_flush,timing_close,NULL};
    struct ps3dk_prof_arc_options arc={4,consume,finish,NULL};
    CHECK(ps3dk_prof_set_sink_callbacks(&cb)==0);CHECK(ps3dk_prof__configure_arcs(&arc)==0);
    CHECK(ps3dk_prof_init(&cfg)==0);
    if(is("before")) {CHECK(export()!=0);CHECK(!stream.export_attempted&&!reads&&!writes&&!closes);}
    if(is("unregistered"))CHECK(ps3dk_prof_thread_begin("main")==0);
    if(is("acked")) {
        CHECK(ps3dk_prof_thread_begin("main")==0);
        CHECK(ps3dk_prof__record_arc(4,8,0)==0);
    }
    if(!is("empty")&&!is("before")&&!is("acked"))for(unsigned i=0;i<(is("reuse")?4u:1u);++i) {
        pthread_t t;CHECK(pthread_create(&t,NULL,worker,is("unregistered")?(void *)1:NULL)==0);
        CHECK(pthread_join(t,NULL)==0);
        if(is("reuse"))CHECK(ps3dk_prof_drain()==0);
    }
    if(is("unregistered"))ps3dk_prof_thread_end();
    if(is("join"))ps3dk_prof_test_hooks.join_fail=1;
    int shutdown=ps3dk_prof_shutdown();
    available=nbody;
    if(is("corrupt"))body[nbody/2]^=1;
    if(is("truncated"))--available;
    if(is("extra"))body[available++]=0;
    if(is("mismatch"))++stream.unregistered_refusals; /* synthetic preflight boundary */
    if(is("overflow"))stream.body_bytes=UINT64_MAX;
    if(is("sequence"))stream.next_sequence=UINT64_MAX;
    if(is("invalid")) {
        CHECK(ps3dk_prof__stream_export(&stream,0,read_body,NULL,&ops,scratch,sizeof scratch)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_export(&stream,1,read_body,NULL,&ops,NULL,0)==PS3DK_PROF_STREAM_INVALID);
        struct ps3dk_prof_arc_output bad=ops;bad.close=NULL;
        CHECK(ps3dk_prof__stream_export(&stream,1,read_body,NULL,&bad,scratch,sizeof scratch)==PS3DK_PROF_STREAM_INVALID);
        CHECK(!stream.export_attempted&&!writes&&!reads&&!closes);
    }
    if(is("reentrant"))reenter=1;
    int rc=export();
    int preflight=is("timing-write")||is("timing-flush")||is("timing-close")||is("join")||is("mismatch")||is("overflow")||is("sequence");
    int failure=preflight||is("output-header")||is("output-body")||is("output-trailer")||is("output-flush")||is("output-close")||is("corrupt")||is("truncated")||is("extra");
    if(preflight)CHECK(rc!=0&&!stream.export_attempted&&!reads&&!writes&&!flushes&&!closes);
    else {
        CHECK((rc!=0)==failure);CHECK(stream.export_attempted&&closes==1);
        CHECK(flushes==(unsigned)(!failure||is("output-flush")||is("output-close")));
        unsigned r=reads,w=writes,f=flushes,c=closes;
        CHECK(export()==PS3DK_PROF_STREAM_STATE);CHECK(reads==r&&writes==w&&flushes==f&&closes==c);
    }
    save(argv[2],"prof",timing,ntiming);save(argv[2],"arcs",output,noutput);
    printf("checks=%u export=%d shutdown=%d bytes=%zu flushes=%u closes=%u\n",checks,rc,shutdown,noutput,flushes,closes);
    return 0;
}
