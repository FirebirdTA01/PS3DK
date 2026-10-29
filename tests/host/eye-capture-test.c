#include <stdio.h>
#include <string.h>
#include "../../samples/vision/hello-ppu-camera/source/eye_capture.h"

static unsigned passed, failed;
#define CHECK(name, expr) do { if (expr) ++passed; else { ++failed; printf("FAIL %s line %d\n", name, __LINE__); } } while (0)
typedef struct Fake {
    char calls[64]; unsigned n, reads, presents, pumps, waits;
    int fail_stage, fail_cleanup, read_error, present_error, pump_error;
    int duplicate, empty, busy, quit, quit_on_present;
    int backwards_frame, backwards_time, backwards_clock;
    uint32_t frame_base;
    uint64_t time, present_delay;
} Fake;
static int control(void *p, enum EyeStage stage)
{
    Fake *f=p; f->calls[f->n++]="LIOSTCEU"[stage];
    return (int)stage==f->fail_stage || (int)stage==f->fail_cleanup ? -99 : 0;
}
static int read_frame(void *p, EyeFrame *frame)
{
    Fake *f=p; static const uint8_t pixel[4]={16,128,16,128};
    ++f->reads; f->time+=1000;
    frame->data=pixel; frame->bytes=f->empty ? 0 : sizeof(pixel);
    frame->number=f->duplicate ? 1 : f->frame_base+f->reads;
    frame->timestamp=f->time;
    if (f->reads==2) {
        if(f->backwards_frame) frame->number-=2;
        if(f->backwards_time) frame->timestamp=0;
        if(f->backwards_clock) f->time=0;
    }
    return f->read_error ? -88 : f->busy ? 1 : 0;
}
static int present(void *p,const EyeFrame *frame)
{ Fake *f=p; (void)frame; ++f->presents; f->time+=f->present_delay; if(f->quit_on_present) f->quit=1; return f->present_error ? -77 : 0; }
static int pump(void *p)
{ Fake *f=p; ++f->pumps; return f->pump_error ? -66 : f->quit; }
static uint64_t now(void *p) { return ((Fake *)p)->time; }
static void wait_frame(void *p) { Fake *f=p; ++f->waits; f->time+=500; }
static Fake initial(void) { Fake f={0}; f.fail_stage=-1; f.fail_cleanup=-1; return f; }
static int run(Fake *f, EyeStats *s, unsigned frames, uint64_t budget)
{
    EyeOps ops={f,control,read_frame,present,pump,now,wait_frame};
    return eye_capture_run(&ops,frames,budget,s);
}
int main(void)
{
    Fake f=initial(); EyeStats s;
    CHECK("success",run(&f,&s,3,30000000)==0 && s.fresh==3 && s.displayed==3 && !s.first_error);
    CHECK("reverse cleanup",!strcmp(f.calls,"LIOSTCEU"));
    CHECK("timestamps",s.first_timestamp==1000 && s.last_timestamp==3000);
    const char *partial[]={"L","LIU","LIOEU","LIOSCEU"};
    for(int stage=EYE_LOAD;stage<=EYE_START;++stage) {
        f=initial(); f.fail_stage=stage;
        CHECK("acquisition failure",run(&f,&s,3,30000000)==-99 && (int)s.error_stage==stage && !strcmp(f.calls,partial[stage]) && !f.reads);
    }
    for(int stage=EYE_STOP;stage<=EYE_UNLOAD;++stage) {
        f=initial(); f.fail_cleanup=stage;
        CHECK("cleanup failure",run(&f,&s,3,30000000)==-99 && (int)s.error_stage==stage && s.cleanup_failures==1 && !strcmp(f.calls,"LIOSTCEU"));
    }
    f=initial(); f.read_error=1; f.fail_cleanup=EYE_STOP;
    CHECK("first error survives cleanup",run(&f,&s,3,30000000)==-88 && s.error_stage==EYE_READ && s.cleanup_failures==1 && !strcmp(f.calls,"LIOSTCEU"));
    f=initial(); f.present_error=1;
    CHECK("presentation failure",run(&f,&s,3,30000000)==-77 && !s.displayed && !strcmp(f.calls,"LIOSTCEU"));
    f=initial(); f.duplicate=1;
    CHECK("duplicate timeout",run(&f,&s,3,30000000)==EYE_TIMEOUT && s.fresh==1 && s.displayed==1 && s.duplicates>0 && f.waits>0 && f.time<2100000);
    f=initial(); f.empty=1;
    CHECK("empty frame refused",run(&f,&s,3,30000000)==EYE_INVALID_FRAME && !f.presents);
    f=initial(); f.busy=1;
    CHECK("busy timeout",run(&f,&s,3,30000000)==EYE_TIMEOUT && !s.fresh && f.waits>0 && f.time<2100000);
    f=initial(); f.present_delay=10000;
    CHECK("total deadline",run(&f,&s,3,5000)==EYE_TIMEOUT && s.displayed==1 && !strcmp(f.calls,"LIOSTCEU"));
    f=initial(); f.quit=1;
    CHECK("user exit",run(&f,&s,3,30000000)==1 && !s.first_error && !f.reads && !strcmp(f.calls,"LIOSTCEU"));
    f=initial(); f.quit=1; f.fail_cleanup=EYE_END;
    CHECK("quit does not hide cleanup failure",run(&f,&s,3,30000000)==-99 && s.cleanup_failures==1);
    f=initial(); f.pump_error=1;
    CHECK("callback failure",run(&f,&s,3,30000000)==-66 && s.error_stage==EYE_PUMP && !f.reads);
    f=initial(); f.quit_on_present=1;
    CHECK("exit during final presentation",run(&f,&s,1,30000000)==1 && s.displayed==1 && !s.first_error && !strcmp(f.calls,"LIOSTCEU"));
    f=initial(); f.backwards_frame=1;
    CHECK("backwards frame refused",run(&f,&s,3,30000000)==EYE_INVALID_FRAME && s.displayed==1);
    f=initial(); f.frame_base=UINT32_MAX-1;
    CHECK("frame wrap accepted",run(&f,&s,3,30000000)==0 && s.displayed==3);
    f=initial(); f.backwards_time=1;
    CHECK("backwards timestamp refused",run(&f,&s,3,30000000)==EYE_INVALID_FRAME && s.displayed==1);
    f=initial(); f.backwards_clock=1;
    CHECK("backwards clock refused",run(&f,&s,3,30000000)==EYE_TIMEOUT && s.displayed==1);
    f=initial();
    CHECK("zero target rejected",run(&f,&s,0,30000000)==EYE_BAD_ARGUMENT && !f.n);
    printf("eye-capture: %u passed, %u failed\n",passed,failed);
    return failed ? 1 : 0;
}
