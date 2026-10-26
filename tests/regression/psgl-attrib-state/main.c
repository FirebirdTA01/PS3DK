#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <Cg/cgGL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include "vertex_vpo.h"
#include "plain_vpo.h"
#include "fragment_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
extern CGprogram psgl_cg_create_program_from_memory(CGcontext, const void *, uint32_t, CGprofile);
static unsigned checks,failures;
static const float quad[24]={-1,-1,0,1, 1,-1,0,1, 1,1,0,1, -1,-1,0,1, 1,1,0,1, -1,1,0,1};
static void check(int ok,const char *name)
{
    ++checks;if(!ok){++failures;printf("PSGL_ATTR_STATE_FAIL %s\n",name);}
}
static GLuint buffer(const void *data,unsigned size)
{
    GLuint b=0;glGenBuffers(1,&b);glBindBuffer(GL_ARRAY_BUFFER,b);
    glBufferData(GL_ARRAY_BUFFER,size,data,GL_STATIC_DRAW);return b;
}
static void draw(unsigned char r,unsigned char g,unsigned char b,const char *name)
{
    unsigned char pixel[4]={0},expected[4]={r,g,b,255};
    glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    unsigned tries;
    for(tries=1;tries<=10;++tries){
        glDrawArrays(GL_TRIANGLES,0,6);
        glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if(!memcmp(pixel,expected,4))break;
        if(tries<10)usleep(200000);
    }
    printf("PSGL_ATTR_STATE_DRAW %s attempts=%u rgba=%02x%02x%02x%02x\n",name,tries>10?10:tries,pixel[0],pixel[1],pixel[2],pixel[3]);
    GLenum error=glGetError();check(!memcmp(pixel,expected,4)&&error==GL_NO_ERROR,name);
}
int main(void)
{
    PSGLinitOptions init={0};psglInit(&init);
    PSGLdevice *dev=psglCreateDeviceAuto(0,0,0);PSGLcontext *ctx=psglCreateContext();
    if(!dev||!ctx){puts("PSGL_ATTR_STATE_FAIL setup");return 1;}
    psglMakeCurrent(ctx,dev);glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_DITHER);
    GLuint pos=buffer(quad,sizeof(quad)),data=0;
#ifdef TEST_NORMALIZATION
    static const unsigned char bytes[24]={255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255,255,0,0,255};
    GLuint vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER),p=glCreateProgram();
    glShaderBinary(1,&vs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,vertex_vpo,vertex_vpo_size);
    glShaderBinary(1,&fs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,fragment_fpo,fragment_fpo_size);
    glAttachShader(p,vs);glAttachShader(p,fs);glBindAttribLocation(p,0,"position");glBindAttribLocation(p,2,"color");glLinkProgram(p);
    GLint status=0;glGetProgramiv(p,GL_LINK_STATUS,&status);glUseProgram(p);
    check(status==GL_TRUE&&glGetError()==GL_NO_ERROR,"normalization program");
    glBindBuffer(GL_ARRAY_BUFFER,pos);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0);glEnableVertexAttribArray(0);
    data=buffer(bytes,sizeof(bytes));glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_TRUE,4,0);draw(1,0,0,"normalized VBO");
    glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_FALSE,4,0);draw(255,0,0,"unnormalized VBO");
    glBindBuffer(GL_ARRAY_BUFFER,0);
    glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_TRUE,4,bytes);draw(1,0,0,"normalized client");
    glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_FALSE,4,bytes);draw(255,0,0,"unnormalized client");
    for(GLint size=1;size<=3;++size){
        glVertexAttribPointer(2,size,GL_UNSIGNED_BYTE,GL_FALSE,4,bytes);
        GLenum error=glGetError();
        check(error==GL_INVALID_OPERATION,"small unnormalized input refused");
        if(error==GL_INVALID_OPERATION){
            draw(255,0,0,"rejected size preserves pointer");
        }else{
            /* The RED must not submit an invalid UB256 descriptor. */
            check(0,"rejected size preserves pointer");
            glVertexAttribPointer(2,4,GL_UNSIGNED_BYTE,GL_FALSE,4,bytes);
        }
    }
    glUseProgram(0);glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);
#else
    static const float red[24]={1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1};
    static const float blue[24]={0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1};
    CGcontext cg=cgCreateContext();
    CGprogram vp=psgl_cg_create_program_from_memory(cg,plain_vpo,plain_vpo_size,CG_PROFILE_SCE_VP_RSX);
    CGprogram fp=psgl_cg_create_program_from_memory(cg,fragment_fpo,fragment_fpo_size,CG_PROFILE_SCE_FP_RSX);
    check(vp&&fp,"disable program");
    if(vp&&fp){
        cgGLBindProgram(vp);cgGLBindProgram(fp);
        glBindBuffer(GL_ARRAY_BUFFER,pos);glVertexPointer(4,GL_FLOAT,16,0);glEnableClientState(GL_VERTEX_ARRAY);
        data=buffer(red,sizeof(red));glColorPointer(4,GL_FLOAT,16,0);glEnableClientState(GL_COLOR_ARRAY);
        draw(255,0,0,"legacy array enabled");
        glDisableClientState(GL_COLOR_ARRAY);glColor4f(0,1,0,1);
        draw(0,255,0,"legacy disabled uses current color");
        glEnableClientState(GL_COLOR_ARRAY);draw(255,0,0,"legacy array reenabled");
        glDisableClientState(GL_COLOR_ARRAY);glBindBuffer(GL_ARRAY_BUFFER,0);
        CGparameter cp=cgGetNamedParameter(vp,"position"),cc=cgGetNamedParameter(vp,"color");
        cgGLSetParameterPointer(cp,4,GL_FLOAT,16,quad);cgGLSetParameterPointer(cc,4,GL_FLOAT,16,blue);
        cgGLEnableClientState(cp);cgGLEnableClientState(cc);draw(0,0,255,"Cg array enabled");
        cgGLDisableClientState(cc);draw(0,255,0,"Cg disabled uses current color");
    }
    cgDestroyContext(cg);
#endif
    glDeleteBuffers(1,&pos);if(data)glDeleteBuffers(1,&data);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_ATTR_STATE_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(ctx);psglDestroyDevice(dev);psglExit();return failures?1:0;
}
