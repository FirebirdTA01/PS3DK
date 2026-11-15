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
 * TEST_WINDOW_MODE 7 (windowhandoffu): the same opened on an unregistered thread (immediate windows never register
 *   their opener); the render thread retires without ending it, and the opener's END closes it at the render
 *   thread's last frame.
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

#if TEST_WINDOW_MODE==5
void ps3dk_prof__auto_test_segment_history(const void *,uint32_t[4]);
void ps3dk_prof__auto_test_unrepresented(uint32_t[3]);
void ps3dk_prof__auto_test_call_brackets(uint64_t[2]);
#endif
#if TEST_WINDOW_MODE==6 || TEST_WINDOW_MODE==7
/* the render thread: five frame marks; in mode 6 it ends the window, in mode 7 it retires leaving it open */
static uint64_t render_frames,render_first;
static int render_end=1;
static void *render_thread(void *unused)
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
/* an unregistered opener: begins WHOLE_RUN, then ends it once the render thread has gone */
static int opened,go,opener_end=1;
static void *opener_thread(void *unused)
{
    (void)unused;
    assert(g_tls.slot1==0 && PS3DK_PROF_CAPTURE_WHOLE_RUN()==PS3DK_PROF_OK && g_tls.slot1==0);
    __atomic_store_n(&opened,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&go,__ATOMIC_ACQUIRE)) {struct timespec ts={0,100000};nanosleep(&ts,NULL);}
    opener_end=PS3DK_PROF_CAPTURE_END();
    return NULL;
}
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
        pthread_t o;
        assert(pthread_create(&o,NULL,opener_thread,NULL)==0);
        while(!__atomic_load_n(&opened,__ATOMIC_ACQUIRE)) {struct timespec ts={0,100000};nanosleep(&ts,NULL);}
        assert(state()==2);
        assert(pthread_create(&t,NULL,render_thread,NULL)==0 && pthread_join(t,NULL)==0);
        assert(render_frames==5 && render_first==0);
        assert(render_end==1 && state()==2);   /* the render thread retired without ending it */
        assert(PS3DK_PROF_CAPTURE_END()==PS3DK_PROF_EPERM);   /* neither its frame thread nor its opener */
        __atomic_store_n(&go,1,__ATOMIC_RELEASE);
        assert(pthread_join(o,NULL)==0);
        assert(opener_end==PS3DK_PROF_OK && state()==0);   /* the opener ended it */
#endif
    }
    complete();
    puts("AUTO window frame-thread handoff PASS");
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
