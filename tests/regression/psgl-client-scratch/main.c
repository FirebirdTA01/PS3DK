/* Hold a queued draw behind a GPU label while an upload recycles its scratch
 * allocation. A PPU thread releases the gate; the recycled upload must wait
 * until the earlier draw has consumed its original bytes. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <cell/gcm.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ppu_thread.h>
#include <sys/process.h>
#include <sys/timer.h>
#include "vertex_vpo.h"
#include "fragment_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);
#define GATE_LABEL 201u
#define DONE_LABEL 206u
#define LAP_VERTICES (3u * 1024u * 1024u / 16u)
#define WRAP_VERTICES (LAP_VERTICES + 4096u)
#define LARGE_VERTICES (5u * 1024u * 1024u / 16u)
#ifndef TEST_GROWTH
#define TEST_GROWTH 0
#endif
#ifndef TEST_TIMEOUT
#define TEST_TIMEOUT 0
#endif
#define GATE_COUNT (TEST_TIMEOUT ? 5u : 1u)
extern unsigned psgl_teardown_wait_timeouts;
static unsigned checks, failures;
static volatile uint32_t *gates[5], *done;
static const float quad[24] = {-1,-1,0,1, 1,-1,0,1, 1,1,0,1,
                              -1,-1,0,1, 1,1,0,1, -1,1,0,1};
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_SCRATCH_FAIL %s\n", name); }
}
static void release_gate(uint64_t delay)
{
    for (unsigned i=0;i<GATE_COUNT;++i) {
        sys_timer_usleep((uint32_t)delay);
        __sync_synchronize(); *gates[i] = 1; __sync_synchronize();
    }
    sys_ppu_thread_exit(0);
}
static int pixel(int x, unsigned char r, unsigned char g, unsigned char b)
{
    unsigned char got[4] = {0xa5,0xa5,0xa5,0xa5};
    const unsigned char want[4] = {r,g,b,255};
    glReadPixels(x,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
    printf("PSGL_SCRATCH_PIXEL x=%d rgba=%02x%02x%02x%02x\n",x,got[0],got[1],got[2],got[3]);
    return glGetError() == GL_NO_ERROR && memcmp(got,want,4) == 0;
}
static void draw(float *colors, unsigned vertices, int x)
{
    glViewport(x,0,4,4);
    glBindBuffer(GL_ARRAY_BUFFER,0);
    glVertexAttribPointer(2,4,GL_FLOAT,GL_FALSE,16,colors);
    glDrawArrays(GL_TRIANGLES,(GLint)vertices-6,6);
}
int main(void)
{
    PSGLinitOptions init={0}; psglInit(&init);
    PSGLdevice *dev=psglCreateDeviceAuto(0,0,0);
    PSGLcontext *ctx=psglCreateContext();
    check(dev && ctx,"device/context");
    if (!dev || !ctx) return 1;
    psglMakeCurrent(ctx,dev);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND); glDisable(GL_DITHER); glDisable(GL_SCISSOR_TEST);
    float *positions=calloc(LARGE_VERTICES,16), *red=calloc(LARGE_VERTICES,16), *blue=calloc(LARGE_VERTICES,16);
    check(positions && red && blue,"fixture allocations");
    if (!positions || !red || !blue) return 1;
    memcpy(positions,quad,sizeof(quad));
    memcpy(positions+(LAP_VERTICES-6)*4,quad,sizeof(quad));
    memcpy(positions+(WRAP_VERTICES-6)*4,quad,sizeof(quad));
    memcpy(positions+(LARGE_VERTICES-6)*4,quad,sizeof(quad));
    for (unsigned i=0;i<LARGE_VERTICES;++i) {
        red[4*i]=red[4*i+3]=1;
        blue[4*i+2]=blue[4*i+3]=1;
    }
    GLuint vb=0,vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER),p=glCreateProgram();
    glShaderBinary(1,&vs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,vertex_vpo,vertex_vpo_size);
    glShaderBinary(1,&fs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,fragment_fpo,fragment_fpo_size);
    glAttachShader(p,vs);glAttachShader(p,fs);
    glBindAttribLocation(p,0,"position");glBindAttribLocation(p,2,"color");glLinkProgram(p);glUseProgram(p);
    GLint linked=0;glGetProgramiv(p,GL_LINK_STATUS,&linked);
    glGenBuffers(1,&vb);glBindBuffer(GL_ARRAY_BUFFER,vb);
    glBufferData(GL_ARRAY_BUFFER,LARGE_VERTICES*16,positions,GL_STATIC_DRAW);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0);glEnableVertexAttribArray(0);glEnableVertexAttribArray(2);
    check(linked && glGetError()==GL_NO_ERROR,"program and position VBO");
    int warm=0;
    for(unsigned i=0;i<10 && !warm;++i) {
        draw(red,6,0);warm=pixel(1,255,0,0);
        if(!warm)sys_timer_usleep(200000);
    }
    check(warm,"shader warmup");
    glFinish();glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);glFinish();
    done=(volatile uint32_t *)cellGcmGetLabelAddress(DONE_LABEL);
    *done=0;
    /* Several short waits hold the draw past PSGL's two-second bound without
       triggering an emulator's recovery from a single long semaphore wait. */
    for(unsigned i=0;i<GATE_COUNT;++i) {
        gates[i]=(volatile uint32_t *)cellGcmGetLabelAddress(GATE_LABEL+i);
        *gates[i]=0;
        cellGcmSetWaitLabel(gCellGcmCurrentContext,GATE_LABEL+i,1);
    }
    __sync_synchronize();
    draw(red,LAP_VERTICES,0);
    cellGcmSetWriteBackEndLabel(gCellGcmCurrentContext,DONE_LABEL,1);
    cellGcmFlush(gCellGcmCurrentContext);
    check(*done==0 && glGetError()==GL_NO_ERROR,"first draw queued behind gate");
    sys_ppu_thread_t thread;
    int started=sys_ppu_thread_create(&thread,release_gate,500000,
        1000,0x4000,SYS_PPU_THREAD_CREATE_JOINABLE,"scratch-gate")==0;
    check(started,"release thread");
    if(!started){for(unsigned i=0;i<GATE_COUNT;++i)*gates[i]=1;__sync_synchronize();glFinish();return 1;}
    unsigned before=psgl_teardown_wait_timeouts;
    uint64_t start=psglGetSystemTime();
    /* Cover the first draw's tail even after bounded warm-up consumed scratch. */
    unsigned vertices=TEST_GROWTH?LARGE_VERTICES:WRAP_VERTICES;
    draw(blue,vertices,4);
    GLenum error=glGetError();unsigned completed=*done;
    unsigned timed_out=psgl_teardown_wait_timeouts-before;
    printf("PSGL_SCRATCH_RECYCLE growth=%d timeout=%d elapsed_us=%llu completed=%u error=%x timeouts=%u\n",
        TEST_GROWTH,TEST_TIMEOUT,(unsigned long long)(psglGetSystemTime()-start),completed,(unsigned)error,timed_out);
    check(error==(TEST_TIMEOUT?GL_OUT_OF_MEMORY:GL_NO_ERROR),"recycle result");
    check(TEST_TIMEOUT?(timed_out==1 && completed==0):(timed_out==0 && completed==1),"completion before recycle or retained timeout");
    uint64_t result=0;sys_ppu_thread_join(thread,&result);glFinish();
    check(pixel(1,255,0,0),"queued red pixels preserved");
    check(TEST_TIMEOUT?pixel(5,0,255,0):pixel(5,0,0,255),"second draw emitted only on success");
    draw(blue,vertices,4);glFinish();
    check(pixel(5,0,0,255),"subsequent upload succeeds");
    glUseProgram(0);glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);glDeleteBuffers(1,&vb);
    free(positions);free(red);free(blue);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_SCRATCH_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(ctx);psglDestroyDevice(dev);psglExit();return failures?1:0;
}
