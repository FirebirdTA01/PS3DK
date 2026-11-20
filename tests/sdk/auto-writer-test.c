#include "auto_writer.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct sink {
    unsigned calls, accepted, fail;
    uint32_t type[8];
    size_t bytes[8];
    uint8_t data[8][AUTO_FRAME_BYTES];
} sink;
static auto_config_store config;
static auto_writer writer;
static sink output;
static uint64_t ranges[10]; /* the capture's range refusals by reason */
static uint8_t scratch[1024];
static int emit(void *user, uint32_t type, const uint8_t *p, size_t n) {
    sink *s=user;
    s->calls++;
    if(s->fail) return -1; /* whole-chunk adapter reports partial acceptance as failure */
    assert(s->accepted<8 && n<=sizeof s->data[0]);
    s->type[s->accepted]=type; s->bytes[s->accepted]=n;
    memcpy(s->data[s->accepted++],p,n);
    return 0;
}
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void setup(int attached) {
    auto_wire_config c;
    uint8_t identity[32]={1};
    memset(&output,0,sizeof output);
    auto_config_init(&config,79800000,identity,identity);
    if(attached) {
        memset(&c,0,sizeof c); c.instance=1;c.domain=1026;c.owner=7;c.epoch=c.generation=1;
        c.pairs=504;
        assert(auto_config_commit(&config,auto_config_begin(&config),&c));
    }
    assert(auto_writer_init(&writer,&config,scratch,sizeof scratch,emit,&output));
}
static void refused(void) {
    auto_wire_op op={0};
    auto_wire_clock clock={0};
    auto_wire_frame frame={0};
    uint64_t counters[15]={0};
    setup(0);
    assert(!auto_writer_begin(&writer) && !output.calls);
    auto_config_stop(&config);
    assert(!auto_writer_begin(&writer));
    assert(output.accepted==2 && output.type[0]==C_AUTO_CONFIG && output.type[1]==C_AUTO_STATUS);
    assert(le32(output.data[0]+24)==AUTO_NO_GCM_INIT);
    assert(output.data[0][2]==AUTO_REFUSED && output.data[1][2]==AUTO_FINAL);
    assert(output.bytes[1]==AUTO_STATUS_REFUSED_BYTES);
    for(unsigned i=8;i<AUTO_STATUS_REFUSED_BYTES;i++) assert(!output.data[1][i]);
    assert(!auto_writer_ops(&writer,&op,1));
    assert(!auto_writer_clock(&writer,&clock));
    assert(!auto_writer_frame(&writer,&frame));
    assert(!auto_writer_status(&writer,AUTO_FINAL,counters,ranges));
    assert(output.accepted==2 && writer.closed && !writer.failed);
}
static void accepted(void) {
    auto_wire_op op={0};
    auto_wire_clock clock={0};
    auto_wire_frame frame={0};
    uint64_t counters[15]={0};
    setup(1);
    assert(!auto_writer_begin(&writer) && !output.calls);
    assert(auto_config_freeze(&config));
    assert(auto_writer_begin(&writer));
    assert(auto_writer_begin(&writer) && output.accepted==1);
    op.id=1;op.serial_end=2;op.published=2;
    clock.instance=1;clock.id=1;
    frame.instance=1;
    assert(auto_writer_ops(&writer,&op,1));
    assert(auto_writer_clock(&writer,&clock));
    assert(auto_writer_frame(&writer,&frame));
    counters[0]=counters[1]=counters[2]=counters[5]=counters[9]=counters[10]=1;
    assert(!auto_writer_status(&writer,0,counters,ranges)); /* publication snapshot must reconcile */
    counters[13]=2;
    auto_config_invalidate(&config,AUTO_STATUS_CONFLICT);
    assert(auto_writer_status(&writer,0,counters,ranges));
    assert(output.data[4][2]==AUTO_STATUS_CONFLICT);
    auto_config_invalidate(&config,AUTO_STATUS_RESET);
    assert(auto_writer_status(&writer,AUTO_FINAL,counters,ranges));
    assert(output.data[5][2]==(AUTO_FINAL|AUTO_STATUS_CONFLICT|AUTO_STATUS_RESET));
    assert(writer.closed && output.accepted==6);
    assert(!auto_writer_ops(&writer,&op,1) && !auto_writer_begin(&writer));
}
static void failed_sink(void) {
    auto_wire_op op={0};
    setup(1); assert(auto_config_freeze(&config));
    output.fail=1;
    assert(!auto_writer_begin(&writer));
    assert(writer.failed && !writer.config_sent);
    output.fail=0;
    assert(!auto_writer_begin(&writer) && output.calls==1);
    setup(1); assert(auto_config_freeze(&config));
    assert(auto_writer_begin(&writer));
    output.fail=1;
    assert(!auto_writer_ops(&writer,&op,1));
    assert(writer.failed && writer.operations_written==0);
    output.fail=0;
    assert(!auto_writer_ops(&writer,&op,1) && output.calls==2);
    setup(0); auto_config_stop(&config); output.fail=1;
    assert(!auto_writer_begin(&writer) && writer.failed && !writer.closed);
}
static void status_refusals(void) {
    uint64_t counters[15]={0};
    setup(1); assert(auto_config_freeze(&config)); assert(auto_writer_begin(&writer));
    counters[2]=1;
    assert(!auto_writer_status(&writer,0,counters,ranges)); /* written must equal accepted OPS */
    assert(output.accepted==1 && !writer.failed);
    counters[2]=0;counters[1]=1;
    assert(!auto_writer_status(&writer,0,counters,ranges)); /* exact conservation */
    counters[0]=counters[4]=1;
    assert(!auto_writer_status(&writer,AUTO_FINAL,counters,ranges)); /* exact final pending */
    assert(auto_writer_status(&writer,AUTO_LOWER_BOUND,counters,ranges));
    counters[0]=counters[1]=counters[4]=0;
    assert(!auto_writer_status(&writer,AUTO_LOWER_BOUND,counters,ranges)); /* regression */
    setup(1); assert(auto_config_freeze(&config)); assert(auto_writer_begin(&writer));
    memset(counters,0,sizeof counters); counters[14]=505;
    assert(!auto_writer_status(&writer,AUTO_LOWER_BOUND,counters,ranges));
    counters[14]=0;
    assert(!auto_writer_status(&writer,4096,counters,ranges)); /* 64..2048 are defined (segments, frame marks, frame sources, replay, pre-init options ignored); 4096 is unknown */
    assert(!auto_writer_status(&writer,AUTO_OWNER_UNACK,counters,ranges));
    assert(!auto_writer_status(&writer,AUTO_SATURATED,counters,ranges));
}
/* SEGMENT_REPLAY is defined and sticky: a later status without it still carries it. */
static void replay_sticky(void) {
    uint64_t counters[15]={0};
    setup(1); assert(auto_config_freeze(&config)); assert(auto_writer_begin(&writer));
    assert(auto_writer_status(&writer,AUTO_STATUS_SEGMENT_REPLAY,counters,ranges));
    assert(auto_writer_status(&writer,AUTO_FINAL,counters,ranges));
    assert(output.accepted==3 && output.type[2]==C_AUTO_STATUS);
    assert((output.data[2][2]|output.data[2][3]<<8)==(AUTO_FINAL|AUTO_STATUS_SEGMENT_REPLAY));
}
/* RANGE_REASONS: each STATUS total is at least the prior STATUS's and the rows' sum of its reason (unless SATURATED);
 * the totals land at 128, slot r-1 for reason r. */
static void range_totals(void) {
    auto_wire_frame frame={0};
    uint64_t counters[15]={0};
    setup(1); assert(auto_config_freeze(&config)); assert(auto_writer_begin(&writer));
    memset(ranges,0,sizeof ranges);
    frame.instance=1;frame.ranges_refused=3;frame.range_refused[1]=2;frame.range_refused[6]=1;
    assert(auto_writer_frame(&writer,&frame));
    counters[5]=1;
    assert(!auto_writer_status(&writer,0,counters,NULL));                  /* totals are required */
    {int below_rows=auto_writer_status(&writer,0,counters,ranges);assert(!below_rows);} /* below the row's reasons */
    ranges[1]=2;
    assert(!auto_writer_status(&writer,0,counters,ranges));                /* reason 7 still below its row */
    ranges[6]=1;ranges[3]=4;                                               /* NO_FRAME counts in no row */
    assert(auto_writer_status(&writer,0,counters,ranges));
    assert(output.bytes[2]==AUTO_STATUS_BYTES && le32(output.data[2]+128+8)==2 && le32(output.data[2]+128+24)==4 &&
           le32(output.data[2]+128+48)==1);
    for(unsigned i=128+80;i<AUTO_STATUS_BYTES;i++) assert(!output.data[2][i]);
    ranges[3]=3;
    {int decreased=auto_writer_status(&writer,0,counters,ranges);assert(!decreased);} /* a total decreased */
    ranges[3]=4;ranges[1]=0;
    assert(!auto_writer_status(&writer,AUTO_SATURATED|AUTO_LOWER_BOUND,counters,ranges)); /* decreased, even saturated */
    ranges[1]=2;
    assert(auto_writer_status(&writer,AUTO_FINAL,counters,ranges));
    memset(ranges,0,sizeof ranges);
}
int main(void) {
    refused();accepted();failed_sink();status_refusals();replay_sticky();range_totals();
    puts("AUTO writer config-first/refusal/final/whole-sink accounting PASS");
    return 0;
}
