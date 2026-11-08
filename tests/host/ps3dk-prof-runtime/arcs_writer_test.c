/* Independent byte fixture/checker lives in tools/ps3dk-prof/tests/arcs_support.
 * No hooks/collector: this tests the allocation-free target serializer. */
#include "prof_arcs_writer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct sink { unsigned char bytes[8192]; size_t used, limit; };
static int write_bytes(void *ctx, const void *bytes, size_t n) {
    struct sink *s = ctx;
    if (n > s->limit - s->used) return -5;
    memcpy(s->bytes + s->used, bytes, n); s->used += n; return 0;
}
static int checks, fails;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); } } while (0)
int main(int argc, char **argv) {
    struct ps3prof_arc_row row = {0x1004,0x2004,7,0};
    struct ps3prof_arc_thread thread = {1,0,42,"worker",&row,1,2,0,7,7,0,0};
    struct ps3prof_arc_snapshot snapshot = {
        "app",(const unsigned char *)"build",5,"sdk","gcc","-pg","work",
        1,1,1,17,1,0,UINT64_MAX,0,2,16,1000,10,&thread,1,0,0
    };
    struct sink sink = {{0},0,8192};
    CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==0);
    const size_t length=sink.used;
    CHECK(length>16 && memcmp(sink.bytes,"PS3DKPRF",8)==0 && sink.bytes[12]==1);
    printf("ARCS_HEX "); for(size_t i=0;i<length;++i) printf("%02x",sink.bytes[i]); printf("\n");
    if(argc==2) {FILE *f=fopen(argv[1],"wb");CHECK(f!=NULL);if(f){CHECK(fwrite(sink.bytes,1,length,f)==length);CHECK(fclose(f)==0);}}
    for(size_t limit=0;limit<length;++limit) {
        sink.used=0;sink.limit=limit;
        CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_SINK);
        CHECK(sink.used<=limit);
    }
    sink.used=0;sink.limit=8192;
    row.count=0;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);CHECK(sink.used==0);row.count=7;
    thread.recorded=8;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);CHECK(sink.used==0);thread.recorded=7;
    row.count=UINT64_MAX;thread.attempted=thread.recorded=UINT64_MAX;
    CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==0);
    sink.used=0;thread.refused=1;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);CHECK(sink.used==0);
    thread.refused=0;thread.state=3;thread.limit_flags=2;thread.lost=thread.refused=UINT64_MAX;
    CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==0);
    sink.used=0;thread.attempted=0;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);CHECK(sink.used==0);
    row.count=7;thread.state=2;thread.limit_flags=0;thread.attempted=thread.recorded=7;thread.lost=thread.refused=0;
    row.flags=2;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);row.flags=0;
    row.caller_return_pc=1;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);row.caller_return_pc=0x1004;
    row.callee_hook_pc=0;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);row.callee_hook_pc=0x2004;
    thread.name="\xc0\x80";CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);thread.name="worker";
    snapshot.mode=2;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);snapshot.mode=1;
    snapshot.snapshot_flags=2;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);snapshot.snapshot_flags=1;
    snapshot.max_arcs_per_thread=0;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);snapshot.max_arcs_per_thread=16;
    snapshot.first_sink_error=-5;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);snapshot.first_sink_error=0;
    thread.limit_flags=2;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);thread.limit_flags=0;
    CHECK(sink.used==0);
    struct ps3prof_arc_row duplicate[2]={row,row};thread.rows=duplicate;thread.row_count=2;
    CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);thread.rows=&row;thread.row_count=1;
    struct ps3prof_arc_thread same_slot[2]={thread,thread};same_slot[1].incarnation=2;snapshot.threads=same_slot;snapshot.thread_count=2;
    CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==PS3PROF_ARCS_INVALID);
    same_slot[0].state=1;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==0);
    snapshot.threads=&thread;snapshot.thread_count=1;sink.used=0;
    thread.limit_flags=1;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==0);thread.limit_flags=0;sink.used=0;
    snapshot.sink_error_count=1;snapshot.first_sink_error=-5;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==0);
    snapshot.sink_error_count=0;snapshot.first_sink_error=0;sink.used=0;
    snapshot.build_id=NULL;snapshot.build_id_len=0;CHECK(ps3prof_arcs_write(&snapshot,write_bytes,&sink)==0);
    printf("writer tests=%d pass=%d fail=%d\n",checks,checks-fails,fails);
    return fails?1:0;
}
