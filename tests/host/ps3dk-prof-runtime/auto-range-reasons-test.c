/* AUTO range refusals by reason on the real runtime with synthetic device writes (CONFIG 3071/11). Nothing completes
 * before the end (no label is written), so held pairs and metadata entries stay held. Every refusal path is driven,
 * and after each step the capture total of exactly that reason and the open row's count move by one (NO_FRAME: the
 * total only, there is no row). The driver then checks the COVERAGE tails and STATUS totals on the wire and through
 * the reader.
 *
 * Options: 8 metadata records, a range reserve of 4 pairs.
 * frame 0: NO_DESCRIPTOR x4 (nameless BEGINs, still pushed) then DEPTH (the fifth nested BEGIN); SEGMENT_BUILD (BEGIN
 *          with a part open); STATE (BEGIN while a segment emitter is active); RING (BEGIN without room for its
 *          report); NO_DESCRIPTOR; UNMATCHED_END; a range opened here (pair 1) ends in frame 1.
 * frame 1: OTHER_FRAME (that END); clears until the metadata is full; NO_METADATA (a range on pair 2 whose END finds
 *          no entry); two ranges on pairs 3 and 4 and a third BEGIN refused NO_PAIR, the two ENDs NO_METADATA.
 * after frame 1 ends: NO_FRAME (BEGIN with no coverage frame open).
 * Totals: depth 1, segment_build 1, state 1, no_frame 1, ring 1, no_descriptor 5, no_pair 1, other_frame 1,
 * no_metadata 3, unmatched_end 1; frame 0's row 10 (depth 1, segment_build 1, state 1, ring 1, no_descriptor 5,
 * unmatched_end 1), frame 1's row 5 (no_pair 1, other_frame 1, no_metadata 3).
 * TEST_RANGE_SATURATE (rangesat): frame 0's DEPTH count is preset to UINT32_MAX - 1 (and the row total with it); the
 * DEPTH refusal reaches UINT32_MAX and marks the row SATURATED (and the capture SATURATED), and a second one sticks
 * there while its capture total still counts. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
#ifndef TEST_RANGE_SATURATE
#define TEST_RANGE_SATURATE 0
#endif
static uint64_t reports[2048][2];
static uint32_t labels[256],words[16384],seg[1024],flushes;
static gpp_ctx context,key,port;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void flush(gpp_ctx *ctx) {
    assert(ctx==&context && ++flushes==1);
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    st64(&reports[1024][0],10000);st32(&labels[246],2);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);
void ps3dk_prof__auto_test_ranges(uint64_t out[4]);
void ps3dk_prof__auto_test_range_reasons(uint64_t total[10],uint32_t row[10],uint32_t out[2]);
void ps3dk_prof__auto_test_segment_active(int on);
void ps3dk_prof__auto_test_row_reason(uint32_t reason,uint32_t value,uint32_t total);
static ps3dk_prof_desc pass={"pass","",__FILE__,3u,2u,0u,0u};
enum {DEPTH=1,SEGMENT,STATE,NO_FRAME,RING,DESCRIPTOR,NO_PAIR,OTHER_FRAME,METADATA,UNMATCHED};

/* One clear (6 words) through c; returns whether it was timed. */
static int clear(gpp_ctx *c)
{
    ps3tc_auto_call call={3,5,0,3,0,0,0,0xf3u};
    uint32_t ticket=ps3dk_prof__auto_begin((void *)c,&call,0x1234);
    for(unsigned n=0;n<6;n++) *c->current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)c,ticket);
    return ticket!=0;
}

static uint64_t total[10],was_total[10];
static uint32_t row[10],was_row[10],state[2];
static void before(void) {ps3dk_prof__auto_test_range_reasons(was_total,was_row,state);}
/* exactly one refusal of reason r (0: none) since before(): in the totals, and in the open row unless in_row is 0 */
static void moved(unsigned r,int in_row)
{
    ps3dk_prof__auto_test_range_reasons(total,row,state);
    for(unsigned i=0;i<10;i++) {
        assert(total[i]==was_total[i]+(i+1==r));
        assert(row[i]==was_row[i]+(in_row && i+1==r));
    }
}

int main(void) {
    ps3dk_prof_auto_options o=PS3DK_PROF_AUTO_OPTIONS_INIT;
    uint64_t r[4];
    unsigned timed=0;
    context=(gpp_ctx){words,words+16384,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    o.metadata_records=8;o.range_pairs=4;
    assert(ps3dk_prof_auto_options_set(&o)==PS3DK_PROF_OK);
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);

    ps3dk_prof_frame(); /* frame 0 */
    assert(clear(&context));
#if TEST_RANGE_SATURATE
    ps3dk_prof__auto_test_row_reason(DEPTH,UINT32_MAX-1u,UINT32_MAX-1u);
#endif
    /* four nameless BEGINs (NO_DESCRIPTOR, still pushed), then DEPTH; the ENDs stay balanced and count nothing */
    for(unsigned i=0;i<4;i++) {before();assert(!ps3dk_prof_auto_range_begin(NULL,0));moved(DESCRIPTOR,1);}
    before();assert(!ps3dk_prof_auto_range_begin(&pass,0));moved(DEPTH,1);
    for(unsigned i=0;i<5;i++) {before();assert(!ps3dk_prof_auto_range_end());moved(0,1);}
#if TEST_RANGE_SATURATE
    /* the DEPTH count reached UINT32_MAX: the row is SATURATED; one more sticks there, its capture total counts */
    assert(row[DEPTH-1]==UINT32_MAX && (state[1]&4u));
    for(unsigned i=0;i<4;i++) assert(!ps3dk_prof_auto_range_begin(NULL,0));
    before();assert(!ps3dk_prof_auto_range_begin(&pass,0));
    ps3dk_prof__auto_test_range_reasons(total,row,state);
    assert(total[DEPTH-1]==was_total[DEPTH-1]+1 && row[DEPTH-1]==UINT32_MAX && (state[1]&4u));
    for(unsigned i=0;i<5;i++) assert(!ps3dk_prof_auto_range_end());
#endif
    /* SEGMENT_BUILD: BEGIN while a part is open */
    ps3dk_prof__auto_segment_external_open((void *)&key,seg,sizeof seg);
    port=(gpp_ctx){seg,seg+1023,seg,NULL};
    assert(clear(&port));
    before();assert(!ps3dk_prof_auto_range_begin(&pass,0));moved(SEGMENT,1);
    before();assert(!ps3dk_prof_auto_range_end());moved(0,1);
    ps3dk_prof__auto_segment_external_close((void *)&key);
    /* STATE: BEGIN while a segment emitter is active (the capture admits nothing) */
    ps3dk_prof__auto_test_segment_active(1);
    before();assert(!ps3dk_prof_auto_range_begin(&pass,0));moved(STATE,1);
    ps3dk_prof__auto_test_segment_active(0);
    assert(!ps3dk_prof_auto_range_end());
    /* RING: no room for the BEGIN report */
    {
        uint32_t *end=context.end;
        context.end=context.current+1;
        before();assert(!ps3dk_prof_auto_range_begin(&pass,0));moved(RING,1);
        context.end=end;
        assert(!ps3dk_prof_auto_range_end());
    }
    /* NO_DESCRIPTOR: no name */
    before();assert(!ps3dk_prof_auto_range_begin(NULL,0));moved(DESCRIPTOR,1);
    assert(!ps3dk_prof_auto_range_end());
    /* UNMATCHED_END */
    before();assert(!ps3dk_prof_auto_range_end());moved(UNMATCHED,1);
    ps3dk_prof__auto_test_ranges(r);assert(r[0]==0 && r[2]==0);
    /* a range across the boundary (pair 1): OTHER_FRAME at its END, counted in frame 1 */
    assert(ps3dk_prof_auto_range_begin(&pass,0));
    ps3dk_prof_frame(); /* frame 1 */
    before();assert(!ps3dk_prof_auto_range_end());moved(OTHER_FRAME,1);
    /* the metadata full: a clear is left untimed */
    for(unsigned i=0;i<16 && clear(&context);i++) timed++;
    assert(timed<16 && !clear(&context));
    /* NO_METADATA: an admitted range (pair 2) whose END finds no metadata entry */
    assert(ps3dk_prof_auto_range_begin(&pass,0));
    before();assert(!ps3dk_prof_auto_range_end());moved(METADATA,1);
    /* NO_PAIR: pairs 3 and 4 admitted, the third BEGIN refused; the refused one's END counts nothing, the other two
     * find no metadata entry */
    assert(ps3dk_prof_auto_range_begin(&pass,0) && ps3dk_prof_auto_range_begin(&pass,0));
    before();assert(!ps3dk_prof_auto_range_begin(&pass,0));moved(NO_PAIR,1);
    before();assert(!ps3dk_prof_auto_range_end());moved(0,1);
    before();assert(!ps3dk_prof_auto_range_end());moved(METADATA,1);
    before();assert(!ps3dk_prof_auto_range_end());moved(METADATA,1);
    ps3dk_prof__auto_test_ranges(r);assert(r[0]==0 && r[2]==0);
    (void)ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token());
    /* NO_FRAME: BEGIN with no coverage frame open; counted in the totals only */
    before();assert(!state[0]);
    assert(!ps3dk_prof_auto_range_begin(&pass,0));moved(NO_FRAME,0);
    assert(!state[0]);
    assert(!ps3dk_prof_auto_range_end());
    ps3dk_prof__auto_test_range_reasons(total,row,state);
    {
        static const uint64_t want[10]={1+TEST_RANGE_SATURATE,1,1,1,1,5+4*TEST_RANGE_SATURATE,1,1,3,1};
        for(unsigned i=0;i<10;i++) assert(total[i]==want[i]);
    }

    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned n=1040;n<2048;n++) st64(&reports[n][0],1000+10*n);
    st32(&labels[247],frontier);
    printf("AUTO range refusals by reason PASS\n");
}
