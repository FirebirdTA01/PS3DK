#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <Cg/cgGL.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
/* Read-only test observation for the same-object Cg bind and routing checks.
 * All binding and mutation uses the public API. */
#include "../../../sdk/libPSGL/src/psgl_context.h"
#include "vertex_vpo.h"
#include "direct_vpo.h"
#include "fragment_fpo.h"
#include "yellow_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
static unsigned checks, failures;
static GLuint program, fs;
static PSGLcontext *context;
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_RELINK_FAIL %s\n", name); }
}
static void pixel(unsigned char r, unsigned char g, unsigned char b, const char *name)
{
    unsigned char got[4]={0}, want[4]={r,g,b,255};
    unsigned tries;
    for (tries=1;tries<=10;++tries) {
        glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES,0,6);
        glReadPixels(2,2,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
        if (!memcmp(got,want,4)) break;
        if (tries<10) usleep(200000);
    }
    printf("PSGL_RELINK_DRAW %s attempts=%u pixel=%02x%02x%02x%02x\n",
           name,tries>10?10:tries,got[0],got[1],got[2],got[3]);
    check(!memcmp(got,want,4) && glGetError()==GL_NO_ERROR,name);
}
static void link_ok(void)
{
    GLint status=0;glLinkProgram(program);glGetProgramiv(program,GL_LINK_STATUS,&status);
    check(status==GL_TRUE,"successful link");
}
static void tint(float r,float g,float b)
{
    GLint location=glGetUniformLocation(program,"tint");
    check(location>=0,"tint location");glUniform4f(location,r,g,b,1);
}
static GLuint shader(GLenum type,const void *bytes,GLsizei size)
{
    GLuint s=glCreateShader(type);
    glShaderBinary(1,&s,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,bytes,size);return s;
}
int main(void)
{
    static const float quad[24]={-1,-1,0,1, 1,-1,0,1, 1,1,0,1,
                                -1,-1,0,1, 1,1,0,1, -1,1,0,1};
    static const unsigned char texels[2][4]={{255,255,255,255},{0,255,0,255}};
    PSGLinitOptions init={0};psglInit(&init);
    PSGLdevice *device=psglCreateDeviceAuto(0,0,0);context=psglCreateContext();
    if (!device||!context) { puts("PSGL_RELINK_FAIL setup");return 1; }
    psglMakeCurrent(context,device);glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_DITHER);
    GLuint vs=shader(GL_VERTEX_SHADER,vertex_vpo,vertex_vpo_size);
    fs=shader(GL_FRAGMENT_SHADER,fragment_fpo,fragment_fpo_size);
    program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);
    glBindAttribLocation(program,0,"position");link_ok();glUseProgram(program);
    GLuint vbo,tex[2];glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0);glEnableVertexAttribArray(0);
    /* Direct Cg uses legacy POSITION at the same buffer. */
    glVertexPointer(4,GL_FLOAT,16,0);glEnableClientState(GL_VERTEX_ARRAY);
    glGenTextures(2,tex);
    for (unsigned i=0;i<2;++i) {
        glActiveTexture(GL_TEXTURE0+5*i);glBindTexture(GL_TEXTURE_2D,tex[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,texels[i]);
    }
    tint(1,0,0);pixel(255,0,0,"GL red control");
    link_ok();tint(0,1,0);pixel(0,255,0,"active GL relink reinstalled");
    CGcontext cg=cgCreateContext();
    CGprogram vp=cgCreateProgram(cg,CG_OBJECT,(const char *)direct_vpo,CG_PROFILE_UNKNOWN,NULL,NULL);
    CGprogram fp=cgCreateProgram(cg,CG_OBJECT,(const char *)yellow_fpo,CG_PROFILE_UNKNOWN,NULL,NULL);
    check(vp&&fp,"public Cg memory fixtures");
    if (!vp||!fp) return 1;
    cgGLLoadProgram(vp);cgGLLoadProgram(fp);
    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX);cgGLBindProgram(vp);
    pixel(0,0,255,"direct VP control");
    link_ok();check(context->bound_vertex_program==vp && !context->gl_attrib_routing,
                    "relink preserves direct VP and routing");
    pixel(0,0,255,"direct VP survives relink");
    tint(1,0,0);glUseProgram(program);pixel(255,0,0,"restore emits current GL numeric uniform");
    cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);cgGLBindProgram(fp);
    pixel(255,255,0,"direct FP control");
    link_ok();check(context->bound_fragment_program==fp && !context->gl_sampler_routing,
                    "relink preserves direct FP and routing");
    pixel(255,255,0,"direct FP survives relink");
    tint(1,1,1);
    GLint image=glGetUniformLocation(program,"image");check(image>=0,"image location");
    glUniform1i(image,5);pixel(255,255,0,"GL sampler setter preserves direct FP");
    glUseProgram(program);pixel(0,255,0,"restore emits current GL sampler selection");
    /* Pointer equality alone cannot distinguish an explicitly rebound stage. */
    CGprogram same=context->bound_vertex_program;
    cgGLBindProgram(same);check(!context->gl_attrib_routing,"same-object Cg clears GL routing");
    link_ok();check(context->bound_vertex_program==same && !context->gl_attrib_routing,
                    "same-object relink keeps Cg routing");
    glUseProgram(program);pixel(255,255,255,"explicit restore resets sampler after link");
    cgGLBindProgram(fp);glDetachShader(program,fs);glLinkProgram(program);
    GLint status=1;glGetProgramiv(program,GL_LINK_STATUS,&status);check(!status,"failed link reported");
    check(context->bound_fragment_program==fp,"failed relink preserves direct FP");
    pixel(255,255,0,"failed relink pixel preserved");
    glAttachShader(program,fs);link_ok();pixel(255,255,0,"repaired link still preserves Cg");
    glUseProgram(program);tint(1,0,0);pixel(255,0,0,"explicit GL restoration after repaired link");
    glUseProgram(0);cgGLUnbindProgram(CG_PROFILE_SCE_VP_RSX);cgGLUnbindProgram(CG_PROFILE_SCE_FP_RSX);
    cgDestroyContext(cg);glDeleteProgram(program);glDeleteShader(vs);glDeleteShader(fs);
    glDeleteBuffers(1,&vbo);glDeleteTextures(2,tex);check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_RELINK_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(context);psglDestroyDevice(device);psglExit();return failures?1:0;
}
