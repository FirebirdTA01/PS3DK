/* Capture windows on the real runtime with synthetic device writes (file_flags bit 9). The runtime records nothing heavy
 * (zones, AUTO draw timing, rows, FRAME marks) outside a window; frame marks keep their run-wide numbering. The driver
 * then checks the WINDOW records, the zone records and the AUTO records of the capture.
 *
 * TEST_WINDOW_MODE 0 (case windows) and 1 (windowseal):
 *   frame 0: a zone and a clear, no window: nothing recorded; CAPTURE_BEGIN (opens at the next boundary), a second
 *            BEGIN refused EBUSY.
 *   frame 1: the window is open: a zone and a timed clear; CAPTURE_END (closes after this frame).
 *   frame 2: closed again: nothing.
 *   frames 3-5: CAPTURE_FRAMES("three", 3) begun in frame 2, open 3..5, closed with COUNT at the boundary of 6.
 *   frame 6: a SECTION around a zone and a clear (begins and ends at its call sites); a second SECTION in the same
 *            frame is refused (windows never share a frame).
 *   frame 7: CAPTURE_BEGIN("tail"); frame 8 open; left open: the capture's end closes it with SHUTDOWN (mode 0) or,
 *            through ps3dk_prof_capture_seal, SEAL (mode 1).
 * TEST_WINDOW_MODE 2 (windowwhole): CAPTURE_WHOLE_RUN, then frames 0-4 each with a zone and a clear: one window
 *   holding every frame's records, as the earlier always-on capture held them.
 * TEST_WINDOW_MODE 4 (windownone): no trigger: five frame marks and nothing heavy.
 * TEST_WINDOW_MODE 5 (windowprebuild): a segment built while no window is open, and one whose build crosses the
 *   window's opening boundary, keep their draw accounting; each CALL inside the window gets a whole-CALL bracket or
 *   counts its draws as unrepresented, never zero.
 * TEST_WINDOW_MODE 6 (windowhandoff): WHOLE_RUN opened on a REGISTERED thread that never marks frames; a render
 *   thread marking five frames owns the window: its state shows the frames and its END succeeds.
 * TEST_WINDOW_MODE 7 (windowhandoffu): WHOLE_RUN opened on an unregistered thread (immediate windows never register
 *   their opener); a render thread marks five frames and owns it.  While that owner lives, an unrelated unregistered
 *   thread's END and the registered main thread's END are refused (EPERM) and the opener's END succeeds; the opener
 *   then begins a second window, the render thread retires without ending it, and the opener's END closes it at the
 *   render thread's last frame.
 * TEST_WINDOW_MODE 9 (windowcallafter): a timed segment built inside WHOLE_RUN, the window ended, then the segment
 *   CALLed before the next frame boundary: nothing is emitted for it and it counts as unrepresented, never as a timed
 *   record in the closed window; a CALL of the same segment inside a later window counts as unrepresented too.
 * TEST_WINDOW_MODE 10 (windowlate): windows opened mid-frame. A WHOLE_RUN before the first frame refuses a range (no
 *   frame, no row) and holds frame 0 from its boundary (19 ranges); one opened by the owner mid-frame 1 gets a late
 *   row from its first range (19 ranges, OPEN even after an explicit exact close; a range after that close has no
 *   row); one opened mid-frame 2 by an unregistered thread (whose own range is refused and opens nothing) gets its
 *   late row from the owner's first draw.
 * TEST_WINDOW_MODE 8 (windowqueue): with the drainer held, eight windows fill the record queue; the ninth BEGIN is
 *   refused EFULL (counted, nothing opens); after the drain catches up a window opens and closes normally, so the
 *   capture never holds a WINDOW_END without its BEGIN. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <ps3dk/prof.h>
#include <ps3dk/prof_auto.h>
#include "prof_internal.h"
#include "prof_test_hooks.h"
#include "gpu_port_gcm.h"
#include "ps3tc_prof_auto.h"
#ifndef TEST_WINDOW_MODE
#define TEST_WINDOW_MODE 0
#endif
static uint64_t reports[2048][2];
static uint32_t labels[256],words[16384],flushes;
static gpp_ctx context;
static void *report(uint32_t n) {return reports[n];}
static void *label(uint32_t n) {return &labels[n];}
static uint64_t ld64(volatile uint64_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static uint32_t ld32(volatile uint32_t *a) {return __atomic_load_n(a,__ATOMIC_SEQ_CST);}
static void st64(volatile uint64_t *a,uint64_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
static void st32(volatile uint32_t *a,uint32_t v) {__atomic_store_n(a,v,__ATOMIC_SEQ_CST);}
/* each clock anchor (frames 0 and 8 here) flushes once: publish and let the device run it at once */
static void flush(gpp_ctx *ctx) {
    assert(ctx==&context);
    ++flushes;
    uint32_t op=ps3dk_prof__auto_publish_before((void *)ctx);
    ps3dk_prof__auto_publish_after((void *)ctx,op);
    st64(&reports[1024+(flushes-1)%16][0],10000*flushes);st32(&labels[246],2*flushes);
}
static const gpp_dev device={report,label,flush,ld64,st64,ld32,st32};
void ps3dk_prof__auto_gcm_init_result(gpp_ctx *,uint32_t,uint32_t,const void *,int32_t);


/* One clear (6 words) through the ring; returns whether it was timed. */
__attribute__((unused)) static int clear(void)
{
    ps3tc_auto_call call={3,5,0,3,0,0,0,0xf3u};
    uint32_t ticket=ps3dk_prof__auto_begin((void *)&context,&call,0x1234);
    for(unsigned n=0;n<6;n++) *context.current++=0xab000000u+n;
    ps3dk_prof__auto_end((void *)&context,ticket);
    return ticket!=0;
}
/* One zone; returns whether it was recorded (a non-INERT handle). */
__attribute__((unused)) static int zone(void)
{
    ps3dk_prof_zone z=PS3DK_PROF_ZONE_BEGIN("work");
    int live=z.kind==1;
    PS3DK_PROF_ZONE_END(z);
    return live;
}
static ps3dk_prof_capture_info st;
__attribute__((unused)) static uint32_t state(void) {assert(ps3dk_prof_capture_state(&st)==PS3DK_PROF_OK);return st.state;}
static void complete(void)
{
    uint32_t frontier=ps3dk_prof__auto_publish_before((void *)&context);
    ps3dk_prof__auto_publish_after((void *)&context,frontier);
    for(unsigned n=1040;n<2048;n++) st64(&reports[n][0],1000+10*n);
    st32(&labels[247],frontier);
}

#if TEST_WINDOW_MODE==10
void ps3dk_prof__auto_test_ranges(uint64_t out[4]);
void ps3dk_prof__auto_test_unrepresented(uint32_t[3]);
static uint64_t r[4];
static void ranges(void) {ps3dk_prof__auto_test_ranges(r);}
/* an unregistered thread: opens a whole-run window mid-frame; its own range is not the owner's (0, no row) */
static int foreign_rc=-1,foreign_range=-1;
static void *foreign_opener(void *unused)
{
    (void)unused;
    assert(g_tls.slot1==0);
    foreign_rc=PS3DK_PROF_CAPTURE_WHOLE_RUN();
    foreign_range=PS3DK_PROF_AUTO_RANGE_BEGIN("foreign");
    (void)PS3DK_PROF_AUTO_RANGE_END();
    return NULL;
}
#endif
#if TEST_WINDOW_MODE==5 || TEST_WINDOW_MODE==9
void ps3dk_prof__auto_test_segment_history(const void *,uint32_t[4]);
void ps3dk_prof__auto_test_unrepresented(uint32_t[3]);
void ps3dk_prof__auto_test_call_brackets(uint64_t[2]);
#endif
#if TEST_WINDOW_MODE==6 || TEST_WINDOW_MODE==7
/* the render thread: five frame marks; in mode 6 it ends the window, in mode 7 it retires leaving it open */
static uint64_t render_frames,render_first;
__attribute__((unused)) static int render_end=1;
__attribute__((unused)) static void *render_thread(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<5;i++) ps3dk_prof_frame();
    assert(state()==2);
    render_frames=st.frames;render_first=st.first_frame;
#if TEST_WINDOW_MODE==6
    render_end=PS3DK_PROF_CAPTURE_END();
#endif
    ps3dk_prof_thread_end();
    return NULL;
}
#if TEST_WINDOW_MODE==7
/* ordered steps across the threads: each waits for its step, acts, then publishes the next */
static int step7;
static void wait_step(int n) {while(__atomic_load_n(&step7,__ATOMIC_ACQUIRE)<n) {struct timespec ts={0,100000};nanosleep(&ts,NULL);}}
static void go_step(int n) {__atomic_store_n(&step7,n,__ATOMIC_RELEASE);}
static int stranger_end=1,opener_end1=1,opener_end2=1,reopened=1;
/* an unregistered opener: begins WHOLE_RUN, ends it while its owner lives, begins another, ends it after the owner
 * retired */
static void *opener_thread(void *unused)
{
    (void)unused;
    assert(g_tls.slot1==0 && PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK && g_tls.slot1==0);
    go_step(1);
    wait_step(3); opener_end1=PS3DK_PROF_CAPTURE_END(); go_step(4);
    wait_step(6); reopened=PS3DK_PROF_CAPTURE_WHOLE_RUN(); go_step(7);
    wait_step(8); opener_end2=PS3DK_PROF_CAPTURE_END();
    assert(g_tls.slot1==0);
    return NULL;
}
/* the render thread: five frames (it adopts the window), one more after the first window closed, then it retires */
static void *render7(void *unused)
{
    (void)unused;
    for(unsigned i=0;i<5;i++) ps3dk_prof_frame();
    assert(state()==2);
    render_frames=st.frames;render_first=st.first_frame;
    go_step(2);
    wait_step(5); ps3dk_prof_frame(); go_step(6);
    wait_step(7); ps3dk_prof_thread_end();
    return NULL;
}
/* an unrelated unregistered thread */
static void *stranger(void *unused) {(void)unused;assert(g_tls.slot1==0);stranger_end=PS3DK_PROF_CAPTURE_END();return NULL;}
#endif
#endif

int main(void) {
    context=(gpp_ctx){words,words+16384,words,NULL};
    ps3dk_prof_test_hooks.gpu_gpp_dev=&device;
    ps3dk_prof__auto_gcm_init_result(&context,sizeof words,sizeof words,words,0);
#if TEST_WINDOW_MODE<2
    ps3dk_prof_frame(); /* frame 0 */
    assert(!zone() && !clear() && state()==0);
    assert(PS3DK_PROF_CAPTURE_BEGIN("first")==PS3DK_PROF_OK && state()==1);
    assert(PS3DK_PROF_CAPTURE_BEGIN("second")==PS3DK_PROF_EBUSY && state()==1 && st.refused==1);
    assert(!zone() && !clear());                           /* opens at the next boundary, not now */
    ps3dk_prof_frame(); /* frame 1 */
    assert(state()==2 && st.id==1 && st.first_frame==1 && st.frames==1 && !strcmp(st.name,"first"));
    assert(zone() && clear());
    assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK && state()==3);
    assert(zone() && clear());                             /* still the current frame */
    ps3dk_prof_frame(); /* frame 2 */
    assert(state()==0 && !zone() && !clear());
    assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_EINVAL);
    assert(PS3DK_PROF_CAPTURE_FRAMES("three",0)==PS3DK_PROF_EINVAL);
    assert(PS3DK_PROF_CAPTURE_FRAMES("three",3)==PS3DK_PROF_OK && state()==1);
    for(unsigned f=3;f<=5;f++) {
        ps3dk_prof_frame(); /* frames 3, 4, 5 */
        assert(state()==2 && st.id==2 && st.first_frame==3 && st.frames==f-2u);
        assert(zone() && clear());
    }
    ps3dk_prof_frame(); /* frame 6: three frames recorded, the window closed itself */
    assert(state()==0 && !zone() && !clear());
    {
        PS3DK_PROF_SECTION("sect");
        assert(state()==2 && st.id==3 && st.first_frame==6 && st.kind==PS3DK_PROF_WINDOW_SECTION);
        assert(zone() && clear());
    }
    assert(state()==0 && !zone());
    {
        PS3DK_PROF_SECTION("again");                       /* the frame the previous window ended in */
        assert(state()==0 && st.refused==2);
    }
    ps3dk_prof_frame(); /* frame 7 */
    assert(PS3DK_PROF_CAPTURE_BEGIN("tail")==PS3DK_PROF_OK);
    ps3dk_prof_frame(); /* frame 8 */
    assert(state()==2 && st.id==4 && zone() && clear());
    assert(st.windows==4);
    complete();
#if TEST_WINDOW_MODE==1
    assert(ps3dk_prof_capture_seal()==PS3DK_PROF_OK);
#endif
    puts("AUTO capture windows PASS");
#elif TEST_WINDOW_MODE==4
    /* no trigger at all: frame marks only, nothing heavy */
    for(unsigned f=0;f<5;f++) {
        ps3dk_prof_frame();
        assert(!zone() && !clear() && state()==0);
    }
    complete();
    puts("AUTO no window PASS");
#elif TEST_WINDOW_MODE==5
    {
        uint32_t sw[128],sw2[128],h[4],u[3],u0;
        uint64_t b[2],b0;
        gpp_ctx key={sw,sw+128,sw,NULL},key2={sw2,sw2+128,sw2,NULL};
        ps3tc_auto_call op={3,5,0,3,0,0,0,0xf3u};
        ps3dk_prof_frame(); /* frame 0: no window */
        ps3dk_prof__auto_segment_open((void *)&key);
        assert(!ps3dk_prof__auto_begin((void *)&key,&op,0));key.current+=6;
        ps3dk_prof__auto_segment_close((void *)&key);
        ps3dk_prof__auto_test_segment_history(&key,h);
        assert(h[0]==1 && h[1]==1);   /* the untimed draw is a refused visit of the build */
        /* a build that crosses the opening boundary: one draw before the window, one inside it */
        ps3dk_prof__auto_segment_open((void *)&key2);
        assert(!ps3dk_prof__auto_begin((void *)&key2,&op,0));key2.current+=6;
        assert(PS3DK_PROF_CAPTURE_BEGIN("calls")==PS3DK_PROF_OK);
        ps3dk_prof_frame(); /* frame 1: open */
        assert(state()==2);
        {
            uint32_t ticket=ps3dk_prof__auto_begin((void *)&key2,&op,0);
            for(unsigned n=0;n<6;n++) *key2.current++=0xab000000u+n;
            ps3dk_prof__auto_end((void *)&key2,ticket);
        }
        ps3dk_prof__auto_segment_close((void *)&key2);
        ps3dk_prof__auto_test_segment_history(&key2,h);
        assert(h[0]==2 && h[1]==1);
        /* each CALL inside the window: a bracket or unrepresented draws, never an empty build */
        gpp_ctx *calls[2]={&key,&key2};
        for(unsigned c=0;c<2;c++) {
            ps3dk_prof__auto_test_unrepresented(u);ps3dk_prof__auto_test_call_brackets(b);u0=u[0];b0=b[0];
            ps3dk_prof__auto_segment_call_begin((void *)&context,(void *)calls[c]);context.current+=5;
            ps3dk_prof__auto_segment_call((void *)&context,(void *)calls[c]);
            ps3dk_prof__auto_test_unrepresented(u);ps3dk_prof__auto_test_call_brackets(b);
            assert(b[0]>b0 || u[0]>u0);
        }
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK);
        ps3dk_prof_frame();
    }
    complete();
    puts("AUTO pre-window segment build PASS");
#elif TEST_WINDOW_MODE==6 || TEST_WINDOW_MODE==7
    {
        pthread_t t;
#if TEST_WINDOW_MODE==6
        assert(!zone() && g_tls.slot1!=0);   /* the opener is registered (here by the AUTO attach) and marks no frame */
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK && state()==2);
        assert(pthread_create(&t,NULL,render_thread,NULL)==0 && pthread_join(t,NULL)==0);
        assert(render_frames==5 && render_first==0);
        assert(render_end==PS3DK_PROF_OK && state()==0);
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_EINVAL);
#else
        pthread_t o,x;
        assert(pthread_create(&o,NULL,opener_thread,NULL)==0);
        wait_step(1);
        assert(state()==2);
        assert(pthread_create(&t,NULL,render7,NULL)==0);
        wait_step(2);
        assert(render_frames==5 && render_first==0 && state()==2);   /* the render thread owns it now */
        assert(pthread_create(&x,NULL,stranger,NULL)==0 && pthread_join(x,NULL)==0);
        assert(stranger_end==PS3DK_PROF_EPERM);   /* an unrelated unregistered thread */
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_EPERM);   /* the registered main thread: neither owner nor opener */
        go_step(3);
        wait_step(4);
        assert(opener_end1==PS3DK_PROF_OK && state()==0);   /* the opener, while the owner lives */
        go_step(5);
        wait_step(7);
        assert(reopened==PS3DK_PROF_OK && state()==2);
        assert(pthread_join(t,NULL)==0);   /* the render thread retired without ending the second window */
        assert(state()==2);
        go_step(8);
        assert(pthread_join(o,NULL)==0);
        assert(opener_end2==PS3DK_PROF_OK && state()==0);   /* the opener, after the owner retired */
#endif
    }
    complete();
    puts("AUTO window frame-thread handoff PASS");
#elif TEST_WINDOW_MODE==9
    {
        uint32_t sw[128],u[3],u0;
        uint32_t *before;
        uint32_t ticket;
        gpp_ctx key={sw,sw+128,sw,NULL};
        ps3tc_auto_call op={3,5,0,3,0,0,0,0xf3u};
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK);
        ps3dk_prof_frame(); /* frame 0, inside the window */
        ps3dk_prof__auto_segment_open((void *)&key);
        ticket=ps3dk_prof__auto_begin((void *)&key,&op,0);
        assert(ticket);   /* a timed segment draw, built inside the window */
        key.current+=6;
        ps3dk_prof__auto_end((void *)&key,ticket);
        ps3dk_prof__auto_segment_close((void *)&key);
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK && state()==0);   /* closed now, frame 0's row still open */
        ps3dk_prof__auto_test_unrepresented(u);u0=u[0];
        before=(uint32_t *)(void *)context.current;
        ps3dk_prof__auto_segment_call_begin((void *)&context,(void *)&key);
        context.current+=5;   /* the CALL and its retirement label */
        ps3dk_prof__auto_segment_call((void *)&context,(void *)&key);
        assert((uint32_t *)(void *)context.current-before==5);   /* no bracket report and no observer words */
        ps3dk_prof__auto_test_unrepresented(u);
        assert(u[0]>u0);   /* executed without a record: unrepresented, not a timed draw */
        ps3dk_prof_frame(); /* frame 1 */
        assert(PS3DK_PROF_CAPTURE_BEGIN("again")==PS3DK_PROF_OK);
        ps3dk_prof_frame(); /* frame 2: the new window */
        assert(state()==2);
        ps3dk_prof__auto_test_unrepresented(u);u0=u[0];
        ps3dk_prof__auto_segment_call_begin((void *)&context,(void *)&key);
        context.current+=5;
        ps3dk_prof__auto_segment_call((void *)&context,(void *)&key);
        ps3dk_prof__auto_test_unrepresented(u);
        assert(u[0]>u0 && u[2]);   /* a re-CALL in a new window never qualifies: unrepresented, replay latched */
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK);
        ps3dk_prof_frame(); /* frame 3 */
    }
    complete();
    puts("AUTO CALL outside a window PASS");
#elif TEST_WINDOW_MODE==10
    {
        pthread_t t;
        uint32_t u[3];
        /* before the first frame: a whole-run window is open but there is no frame, so no row: the range is refused
         * and counted in the run total only; its END stays balanced */
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK && state()==2);
        assert(!PS3DK_PROF_AUTO_RANGE_BEGIN("early") && !PS3DK_PROF_AUTO_RANGE_END());
        ranges();assert(r[0]==0 && r[1]==1 && r[2]==0);
        ps3dk_prof_frame(); /* frame 0: the window held it from its boundary: an exact row with FRAME marks */
        for(unsigned i=0;i<19;i++) {assert(PS3DK_PROF_AUTO_RANGE_BEGIN("pass") && clear() && PS3DK_PROF_AUTO_RANGE_END());}
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK);
        ps3dk_prof_frame(); /* frame 1: outside a window; the owner opens one mid-frame */
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK);
        ps3dk_prof__auto_test_unrepresented(u);assert(!u[1]);   /* opening the window touched no AUTO state */
        for(unsigned i=0;i<19;i++) {
            assert(PS3DK_PROF_AUTO_RANGE_BEGIN("late owner range"));   /* the first one opens the late row */
            assert(clear() && PS3DK_PROF_AUTO_RANGE_END());
        }
        ranges();assert(r[0]==38 && r[1]==1);
        /* an exact explicit closure of the late row still leaves it OPEN; a later range in that frame has no row */
        assert(ps3dk_prof_auto_frame_end(1,ps3dk_prof_frame_token())==0);
        assert(!PS3DK_PROF_AUTO_RANGE_BEGIN("after close") && !PS3DK_PROF_AUTO_RANGE_END());
        ranges();assert(r[1]==2 && r[2]==0);
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK);
        ps3dk_prof_frame(); /* frame 2: another thread (unregistered) opens the window mid-frame */
        assert(pthread_create(&t,NULL,foreign_opener,NULL)==0 && pthread_join(t,NULL)==0);
        assert(foreign_rc==PS3DK_PROF_OK && foreign_range==0 && state()==2);
        ps3dk_prof__auto_test_unrepresented(u);assert(!u[1]);   /* neither the opener nor its range opened a row */
        assert(clear());                                         /* the owner's first draw opens the late row */
        ps3dk_prof__auto_test_unrepresented(u);assert(u[1]&64);
        assert(PS3DK_PROF_AUTO_RANGE_BEGIN("foreign window") && clear() && PS3DK_PROF_AUTO_RANGE_END());
        assert(PS3DK_PROF_AUTO_RANGE_BEGIN("foreign window 2") && PS3DK_PROF_AUTO_RANGE_END());
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK);
        ps3dk_prof_frame(); /* frame 3: outside, the late row closes */
    }
    complete();
    puts("AUTO late rows PASS");
#elif TEST_WINDOW_MODE==8
    {
        uint32_t expected=0;
        unsigned i;
        while(!CAS_ACQREL(&g_prof->drain_busy,&expected,1u)) expected=0;   /* a drainer that is behind */
        for(i=0;i<8;i++) {
            ps3dk_prof_frame();
            assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK && PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK);
        }
        ps3dk_prof_frame();
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_EFULL);
        assert(state()==0 && st.queue_full==1 && st.refused==1 && st.windows==8 && !zone() && !clear());
        ST_REL(&g_prof->drain_busy,0u);
        for(i=0;i<5000 && LD_ACQ(&g_prof->winq_r)!=LD_ACQ(&g_prof->winq_w);i++) {
            struct timespec ts={0,1000000};
            nanosleep(&ts,NULL);
        }
        assert(LD_ACQ(&g_prof->winq_r)==LD_ACQ(&g_prof->winq_w));
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK && zone());
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_OK);
    }
    complete();
    puts("AUTO window queue PASS");
#else
    {
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK);
        assert(PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_EBUSY);
    }
    for(unsigned f=0;f<5;f++) {
        ps3dk_prof_frame();
        assert(zone() && clear());
    }
    complete();
    puts("AUTO whole-run window PASS");
#endif
}
