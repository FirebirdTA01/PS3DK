/* Real encoder/replay with bounded in-memory I/O and independent driver CRC. */
#include "prof_arcs_stream.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static struct ps3dk_prof_arc_stream stream;
static struct ps3dk_prof_stream_slot slots[2];
static struct ps3dk_prof_stream_final finals[2];
static unsigned char body[1048576], output[1048576], scratch[65536];
static size_t used, available, cursor, delivered, read_cap;
static unsigned reads, writes, read_fail, write_fail, reenter, spool_fail, partial;

static int spool(void *ctx, const void *p, size_t n) {
    (void)ctx;
    CHECK(n<=sizeof body-used);
    if (spool_fail) { memcpy(body+used,p,n/2); used+=n/2; return -1; }
    memcpy(body+used,p,n); used+=n; return 0;
}
static ptrdiff_t input(void *, void *, size_t);
static int sink(void *, const void *, size_t);
static void recursive(void) {
    if (!reenter) return;
    unsigned r=reads,w=writes;
    CHECK(ps3dk_prof__stream_replay(&stream,input,NULL,sink,NULL,scratch,sizeof scratch)==PS3DK_PROF_STREAM_STATE);
    CHECK(reads==r && writes==w);
}
static ptrdiff_t input(void *ctx, void *p, size_t n) {
    (void)ctx; ++reads; CHECK(n>0 && n<=sizeof scratch); recursive();
    if (reads==read_fail) return -1;
    size_t count=available-cursor;
    if (count>n) count=n;
    if (read_cap && count>read_cap) count=read_cap;
    memcpy(p,body+cursor,count); cursor+=count; return (ptrdiff_t)count;
}
static ptrdiff_t overcount(void *ctx, void *p, size_t n) {
    (void)ctx; (void)p; ++reads; return (ptrdiff_t)n+1;
}
static int sink(void *ctx, const void *p, size_t n) {
    (void)ctx; ++writes; recursive(); CHECK(n<=sizeof output-delivered);
    if (writes==write_fail) {
        if (partial) { memcpy(output+delivered,p,n/2); delivered+=n/2; }
        return -1;
    }
    memcpy(output+delivered,p,n); delivered+=n; return 0;
}
static void prepare(unsigned history) {
    memset(&stream,0,sizeof stream); memset(slots,0,sizeof slots); memset(finals,0,sizeof finals);
    used=cursor=delivered=read_cap=0; reads=writes=read_fail=write_fail=reenter=spool_fail=partial=0;
    CHECK(ps3dk_prof__stream_init(&stream,slots,finals,2,8,sizeof body,spool,NULL)==0);
    for (unsigned i=0;i<history;++i) {
        struct ps3prof_arc_row row={.caller_return_pc=4,.callee_hook_pc=8,.count=i+1,.flags=0};
        struct ps3dk_prof_arc_view view={.generation=i+1,.thread={.incarnation=i+1,.slot=0,
            .os_thread_id=42,.name="worker",.rows=&row,.row_count=1,
            .state=(uint8_t)(i+1==history?2:1),.attempted=i+1,.recorded=i+1}};
        CHECK(ps3dk_prof__stream_accept(&stream,&view)==0);
    }
    available=used;
}
static int replay(size_t n) { return ps3dk_prof__stream_replay(&stream,input,NULL,sink,NULL,scratch,n); }
static void finished(void) { CHECK(ps3dk_prof__stream_finish(&stream,0)==0); }
static void no_retry(void) {
    unsigned r=reads,w=writes;
    CHECK(replay(7)==PS3DK_PROF_STREAM_STATE); CHECK(reads==r && writes==w);
}
static void save(const char *out, const char *name) {
    char path[1024]; int n=snprintf(path,sizeof path,"%s/%s.body",out,name);
    CHECK(n>0 && (size_t)n<sizeof path); FILE *f=fopen(path,"wb"); CHECK(f!=NULL);
    CHECK(fwrite(body,1,used,f)==used); CHECK(fclose(f)==0);
    printf("bytes=%llu crc=%u checks=%u\n",(unsigned long long)stream.body_bytes,stream.body_crc32,checks);
}
int main(int argc, char **argv) {
    if (argc!=3) return 2;
    const char *which=argv[1];
    if (!strcmp(which,"empty") || !strcmp(which,"normal") || !strcmp(which,"short") ||
        !strcmp(which,"large") || !strcmp(which,"reentrant")) {
        unsigned history=!strcmp(which,"empty")?0:(!strcmp(which,"large")?4096:4);
        prepare(history); finished();
        if (!strcmp(which,"short")) read_cap=1;
        if (!strcmp(which,"reentrant")) reenter=1;
        CHECK(replay(!strcmp(which,"short")?7:sizeof scratch)==0);
        CHECK(cursor==used && delivered==used && memcmp(body,output,used)==0);
        CHECK(stream.replay_attempted && !stream.error); CHECK(reads>=1); no_retry();
        save(argv[2],which);
    } else if (!strcmp(which,"truncated") || !strcmp(which,"extra") || !strcmp(which,"corrupt")) {
        prepare(4); finished();
        if (!strcmp(which,"truncated")) --available;
        if (!strcmp(which,"extra")) body[available++]=0;
        if (!strcmp(which,"corrupt")) body[used/2]^=1;
        CHECK(replay(13)==PS3DK_PROF_STREAM_IO); CHECK(stream.error==PS3DK_PROF_STREAM_IO); no_retry();
    } else if (!strcmp(which,"read-errors")) {
        prepare(4); finished(); size_t n=used;
        for (unsigned fail=1;fail<=n/13+(n%13!=0)+1;++fail) {
            prepare(4); finished(); read_fail=fail;
            CHECK(replay(13)==PS3DK_PROF_STREAM_IO); CHECK(reads==fail); no_retry();
        }
    } else if (!strcmp(which,"overcount")) {
        prepare(4); finished();
        CHECK(ps3dk_prof__stream_replay(&stream,overcount,NULL,sink,NULL,scratch,13)==PS3DK_PROF_STREAM_IO);
        CHECK(writes==0 && reads==1); no_retry();
    } else if (!strcmp(which,"output-errors")) {
        prepare(4); finished(); size_t n=used;
        for (unsigned fail=1;fail<=n/13+(n%13!=0);++fail) for (unsigned part=0;part<2;++part) {
            prepare(4); finished(); write_fail=fail; partial=part;
            CHECK(replay(13)==PS3DK_PROF_STREAM_IO); CHECK(writes==fail && reads==fail); no_retry();
        }
    } else if (!strcmp(which,"state")) {
        prepare(1); CHECK(replay(13)==PS3DK_PROF_STREAM_STATE); CHECK(!stream.replay_attempted && !reads && !writes);
        finished(); CHECK(replay(13)==0); no_retry();
        prepare(0); spool_fail=1;
        struct ps3dk_prof_arc_view v={.generation=1,.thread={.incarnation=1,.slot=0,.name="x",.state=1}};
        CHECK(ps3dk_prof__stream_accept(&stream,&v)==PS3DK_PROF_STREAM_IO);
        CHECK(stream.body_bytes==0 && stream.body_crc32==0);
        CHECK(ps3dk_prof__stream_finish(&stream,0)==PS3DK_PROF_STREAM_IO);
        CHECK(replay(13)!=0 && reads==0 && writes==0);
    } else if (!strcmp(which,"arguments")) {
        prepare(1); finished();
        CHECK(ps3dk_prof__stream_replay(NULL,input,NULL,sink,NULL,scratch,1)==PS3DK_PROF_STREAM_STATE);
        CHECK(ps3dk_prof__stream_replay(&stream,NULL,NULL,sink,NULL,scratch,1)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_replay(&stream,input,NULL,NULL,NULL,scratch,1)==PS3DK_PROF_STREAM_INVALID);
        CHECK(ps3dk_prof__stream_replay(&stream,input,NULL,sink,NULL,NULL,1)==PS3DK_PROF_STREAM_INVALID);
        CHECK(replay(0)==PS3DK_PROF_STREAM_INVALID); CHECK(replay(65537)==PS3DK_PROF_STREAM_INVALID);
        CHECK(!reads && !writes && !stream.replay_attempted);
        CHECK(replay(1)==0); no_retry();
    } else return 2;
    printf("checks=%u\n",checks); return 0;
}
