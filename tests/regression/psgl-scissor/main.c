#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include "vertex_vpo.h"
#include "white_fpo.h"
#ifndef GL_SCISSOR_BOX
#define GL_SCISSOR_BOX 0x0C10
#endif
SYS_PROCESS_PARAM(1001,0x100000);
static unsigned checks,failures;
static const float quad[24]={-1,-1,0,1,1,-1,0,1,1,1,0,1,-1,-1,0,1,1,1,0,1,-1,1,0,1};
static void check(int ok,const char *name)
{++checks;if(!ok){++failures;printf("PSGL_SCISSOR_FAIL %s\n",name);}}
static void box(GLint x,GLint y,GLint w,GLint h,const char *name)
{
    const GLint want[4]={x,y,w,h};GLint iv[6]={71,71,71,71,71,71};
    GLfloat fv[6]={71,71,71,71,71,71};GLboolean bv[6]={71,71,71,71,71,71};
    glGetIntegerv(GL_SCISSOR_BOX,iv+1);glGetFloatv(GL_SCISSOR_BOX,fv+1);glGetBooleanv(GL_SCISSOR_BOX,bv+1);
    int ok=iv[0]==71&&iv[5]==71&&fv[0]==71&&fv[5]==71&&bv[0]==71&&bv[5]==71;
    for(unsigned i=0;i<4;++i)ok=ok&&iv[i+1]==want[i]&&fv[i+1]==(GLfloat)want[i]&&bv[i+1]==(want[i]!=0);
    check(ok&&glGetError()==GL_NO_ERROR,name);
}
static void enabled(int want,const char *name)
{
    GLint i=-1;GLfloat f=-1;GLboolean b=71;
    glGetIntegerv(GL_SCISSOR_TEST,&i);glGetFloatv(GL_SCISSOR_TEST,&f);glGetBooleanv(GL_SCISSOR_TEST,&b);
    check(i==want&&f==(float)want&&b==want&&glGetError()==GL_NO_ERROR,name);
}
static void reset_black(void)
{glDisable(GL_SCISSOR_TEST);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);}
static void pixels(int x,int y,int w,int h,const char *name)
{
    unsigned char out[8*6*4];memset(out,0xa5,sizeof(out));
    glReadPixels(0,0,8,6,GL_RGBA,GL_UNSIGNED_BYTE,out);
    int ok=glGetError()==GL_NO_ERROR;unsigned wrong=0;
    for(int row=0;row<6;++row)for(int col=0;col<8;++col){
        unsigned p=4*(row*8+col);unsigned char c=col>=x&&col<x+w&&row>=y&&row<y+h?255:0;
        if(out[p]!=c||out[p+1]!=c||out[p+2]!=c||out[p+3]!=255)++wrong;
    }
    if(wrong)printf("PSGL_SCISSOR_PIXELS %s mismatches=%u\n",name,wrong);
    check(ok&&!wrong,name);
}
static void warm(void)
{
    reset_black();glViewport(0,0,8,6);
    unsigned n;unsigned char p[4]={0};
    for(n=1;n<=10;++n){
        glDrawArrays(GL_TRIANGLES,0,6);glReadPixels(3,3,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p);
        if(p[0]==255&&p[1]==255&&p[2]==255&&p[3]==255)break;
        if(n<10)usleep(200000);
    }
    printf("PSGL_SCISSOR_WARM attempts=%u\n",n>10?10:n);
    check(n<=10&&glGetError()==GL_NO_ERROR,"immutable white shader warmup");
}
static void target(const char *name)
{
    printf("PSGL_SCISSOR_TARGET %s\n",name);warm();
    /* Clears deliberately have no intervening draw to emit dirty state. */
    reset_black();glScissor(1,1,3,2);glEnable(GL_SCISSOR_TEST);
    glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);pixels(1,1,3,2,"offset clear lower-left");
    reset_black();glScissor(1,1,3,2);glEnable(GL_SCISSOR_TEST);
    glDrawArrays(GL_TRIANGLES,0,6);pixels(1,1,3,2,"offset draw lower-left");
    reset_black();glScissor(-2,-1,5,3);glEnable(GL_SCISSOR_TEST);
    glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);pixels(0,0,3,2,"negative origin clipped clear");
    box(-2,-1,5,3,"negative origin retained in state");
    reset_black();glScissor(-2,-1,5,3);glEnable(GL_SCISSOR_TEST);
    glDrawArrays(GL_TRIANGLES,0,6);pixels(0,0,3,2,"negative origin clipped draw");
    reset_black();glScissor(2,1,0,3);glEnable(GL_SCISSOR_TEST);
    glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);pixels(0,0,0,0,"zero width clear empty");
    glScissor(2,1,3,0);glDrawArrays(GL_TRIANGLES,0,6);pixels(0,0,0,0,"zero height draw empty");
    reset_black();glScissor(INT_MAX-4,INT_MAX-4,20,20);glEnable(GL_SCISSOR_TEST);
    glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);pixels(0,0,0,0,"overflowing endpoints outside");
    reset_black();glScissor(INT_MIN,INT_MIN,INT_MAX,INT_MAX);glEnable(GL_SCISSOR_TEST);
    glDrawArrays(GL_TRIANGLES,0,6);pixels(0,0,0,0,"large negative endpoints outside");
    reset_black();glScissor(-3,-2,INT_MAX,INT_MAX);glEnable(GL_SCISSOR_TEST);
    glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);pixels(0,0,8,6,"oversize clear covers target");
    reset_black();glScissor(-3,-2,INT_MAX,INT_MAX);glEnable(GL_SCISSOR_TEST);
    glDrawArrays(GL_TRIANGLES,0,6);pixels(0,0,8,6,"oversize draw covers target");
    reset_black();glScissor(2,1,2,3);box(2,1,2,3,"box while disabled");enabled(0,"disabled query");
    glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);pixels(0,0,8,6,"disabled clear ignores box");
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_SCISSOR_TEST);enabled(1,"enabled query");box(2,1,2,3,"reenable retains box");
    glDrawArrays(GL_TRIANGLES,0,6);pixels(2,1,2,3,"reenable applies retained box");
    glScissor(9,9,-1,3);check(glGetError()==GL_INVALID_VALUE,"negative width error");box(2,1,2,3,"negative width preserves box");
    glScissor(9,9,3,-1);check(glGetError()==GL_INVALID_VALUE,"negative height error");box(2,1,2,3,"negative height preserves box");
    glDisable(GL_SCISSOR_TEST);glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);
    pixels(0,0,8,6,"disable after draw restores full clear");
}
int main(void)
{
    PSGLinitOptions init={0};psglInit(&init);PSGLdevice *d=psglCreateDeviceAuto(0,0,0);PSGLcontext *c=psglCreateContext();
    if(!d||!c){puts("PSGL_SCISSOR_FAIL setup");return 1;}psglMakeCurrent(c,d);
    GLuint width=0,height=0;psglGetRenderBufferDimensions(d,&width,&height);
    check(width>=8&&height>=6,"surface dimensions");box(0,0,width,height,"initial surface box");enabled(0,"initial disabled");
    GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER),p=glCreateProgram();
    glShaderBinary(1,&vs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,vertex_vpo,vertex_vpo_size);
    glShaderBinary(1,&fs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,white_fpo,white_fpo_size);
    glAttachShader(p,vs);glAttachShader(p,fs);glBindAttribLocation(p,0,"position");glLinkProgram(p);
    GLint linked=0;glGetProgramiv(p,GL_LINK_STATUS,&linked);check(linked&&glGetError()==GL_NO_ERROR,"shader link");glUseProgram(p);
    GLuint pos;glGenBuffers(1,&pos);glBindBuffer(GL_ARRAY_BUFFER,pos);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0);glEnableVertexAttribArray(0);
    glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_CULL_FACE);glDisable(GL_DITHER);
    target("default");
    GLuint storage,tex,fbo;glGenBuffers(1,&storage);glBindBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE,storage);
    glBufferData(GL_TEXTURE_REFERENCE_BUFFER_SCE,64*6,NULL,GL_STATIC_DRAW);
    glGenTextures(1,&tex);glBindTexture(GL_TEXTURE_2D,tex);glTextureReferenceSCE(GL_TEXTURE_2D,1,8,6,1,GL_RGBA,64,0);
    glGenFramebuffersOES(1,&fbo);glBindFramebufferOES(GL_FRAMEBUFFER_OES,fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_2D,tex,0);
    check(glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES)==GL_FRAMEBUFFER_COMPLETE_OES,"reference FBO complete");
    target("FBO");
    /* Keep the same enabled box while changing target heights, no setter. */
    reset_black();glScissor(1,1,3,2);glEnable(GL_SCISSOR_TEST);glClearColor(1,1,1,1);glClear(GL_COLOR_BUFFER_BIT);
    pixels(1,1,3,2,"FBO switch precursor");
    glBindFramebufferOES(GL_FRAMEBUFFER_OES,0);glClear(GL_COLOR_BUFFER_BIT);
    /* Earlier default content was white; clear black while enabled, then
       inspect that only the retained box changed, using white as background. */
    glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    unsigned char corner[4]={0};glReadPixels(2,1,1,1,GL_RGBA,GL_UNSIGNED_BYTE,corner);
    check(corner[0]==0&&corner[1]==0&&corner[2]==0&&corner[3]==255&&glGetError()==GL_NO_ERROR,"target switch recalculates vertical origin");
    glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,corner);
    check(corner[0]==255&&corner[1]==255&&corner[2]==255&&corner[3]==255&&glGetError()==GL_NO_ERROR,"target switch preserves outside");
    glDisable(GL_SCISSOR_TEST);glUseProgram(0);glDeleteFramebuffersOES(1,&fbo);glDeleteTextures(1,&tex);glDeleteBuffers(1,&storage);
    glDeleteBuffers(1,&pos);glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);
    check(glGetError()==GL_NO_ERROR,"cleanup");printf("PSGL_SCISSOR_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(c);psglDestroyDevice(d);psglExit();return failures?1:0;
}
