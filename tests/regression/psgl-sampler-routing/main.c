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
#include "single0_fpo.h"
#include "single5_fpo.h"
#include "pair_fpo.h"
SYS_PROCESS_PARAM(1001,0x100000);
extern CGprogram psgl_cg_create_program_from_memory(CGcontext,const void *,uint32_t,CGprofile);
static unsigned checks,failures;
static const float quad[24]={-1,-1,0,1,1,-1,0,1,1,1,0,1,-1,-1,0,1,1,1,0,1,-1,1,0,1};
static void check(int ok,const char *name){++checks;if(!ok){++failures;printf("PSGL_SAMPLER_FAIL %s\n",name);}}
static GLuint shader(GLenum stage,const void *bytes,unsigned size)
{
    GLuint s=glCreateShader(stage);glShaderBinary(1,&s,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,bytes,size);return s;
}
static GLuint program(GLuint vs,GLuint fs)
{
    GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);glBindAttribLocation(p,0,"position");glLinkProgram(p);
    GLint linked=0;glGetProgramiv(p,GL_LINK_STATUS,&linked);check(linked&&glGetError()==GL_NO_ERROR,"link");return p;
}
static void draw(unsigned r,unsigned g,unsigned b,const char *name)
{
    unsigned char got[4]={0},want[4]={r,g,b,255};
    glClearColor(0.4f,0.2f,0.8f,1);glClear(GL_COLOR_BUFFER_BIT);
    unsigned n;for(n=1;n<=10;++n){
        glDrawArrays(GL_TRIANGLES,0,6);glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
        if(!memcmp(got,want,4))break;
        if(n<10)usleep(200000);
    }
    printf("PSGL_SAMPLER_DRAW %s attempts=%u rgba=%02x%02x%02x%02x\n",name,n>10?10:n,got[0],got[1],got[2],got[3]);
    check(!memcmp(got,want,4)&&glGetError()==GL_NO_ERROR,name);
}
int main(void)
{
    PSGLinitOptions init={0};psglInit(&init);
    PSGLdevice *d=psglCreateDeviceAuto(0,0,0);PSGLcontext *c=psglCreateContext();
    if(!d||!c){puts("PSGL_SAMPLER_FAIL setup");return 1;}
    psglMakeCurrent(c,d);glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_DITHER);
    GLuint vs=shader(GL_VERTEX_SHADER,vertex_vpo,vertex_vpo_size);
    GLuint fs0=shader(GL_FRAGMENT_SHADER,single0_fpo,single0_fpo_size);
    GLuint fs5=shader(GL_FRAGMENT_SHADER,single5_fpo,single5_fpo_size);
    GLuint fs2=shader(GL_FRAGMENT_SHADER,pair_fpo,pair_fpo_size);
    GLuint pa=program(vs,fs0),pb=program(vs,fs0),p5=program(vs,fs5),pp=program(vs,fs2);
    GLuint pos,tex[2];glGenBuffers(1,&pos);glBindBuffer(GL_ARRAY_BUFFER,pos);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glUseProgram(pa);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0);glEnableVertexAttribArray(0);
    static const unsigned char pixels[2][4]={{204,51,17,255},{51,153,204,255}};
    glGenTextures(2,tex);
    for(unsigned i=0;i<2;++i){
        glActiveTexture(GL_TEXTURE0+5*i);glBindTexture(GL_TEXTURE_2D,tex[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels[i]);
    }
    GLint la=glGetUniformLocation(pa,"image"),lb=glGetUniformLocation(pb,"image"),l5=glGetUniformLocation(p5,"image");
    GLint aa=glGetUniformLocation(pp,"a"),bb=glGetUniformLocation(pp,"b");
    check(la>=0&&lb>=0&&l5>=0&&aa>=0&&bb>=0,"sampler locations");
    draw(204,51,17,"default zero");
    glUniform1i(la,5);draw(51,153,204,"physical0 maps logical5");
    glUniform1i(la,0);draw(204,51,17,"restore logical0");
    glUniform1i(la,5);
    glUseProgram(pb);draw(204,51,17,"shared shader independent default");
    glUseProgram(pa);draw(51,153,204,"switch restores selection");
    glUniform1i(lb,0);check(glGetError()==GL_INVALID_OPERATION,"foreign location rejected");draw(51,153,204,"foreign setter preserves selection");
    glUniform1i(la,-1);check(glGetError()==GL_INVALID_VALUE,"negative unit rejected");draw(51,153,204,"negative preserves selection");
    glUniform1i(la,8);check(glGetError()==GL_INVALID_VALUE,"upper unit rejected");draw(51,153,204,"upper preserves selection");
    glUniform1f(la,0);check(glGetError()==GL_INVALID_OPERATION,"float sampler setter rejected");draw(51,153,204,"float setter preserves selection");
    glUniform1i(-1,8);check(glGetError()==GL_NO_ERROR,"minus one location ignored");draw(51,153,204,"ignored setter preserves selection");
    glUseProgram(p5);draw(204,51,17,"physical5 defaults logical0");
    glUniform1i(l5,5);draw(51,153,204,"physical5 selects logical5");
    glUseProgram(pp);glUniform1i(aa,5);glUniform1i(bb,0);draw(51,51,17,"two sampler cross map");
    glUniform1i(bb,5);draw(51,153,204,"same type shared logical unit");
    glActiveTexture(GL_TEXTURE5);glBindTexture(GL_TEXTURE_2D,0);draw(0,0,0,"unbound logical texture black");
    glBindTexture(GL_TEXTURE_2D,tex[1]);draw(51,153,204,"logical rebind updates both slots");
    glUseProgram(pa);glDetachShader(pa,fs0);glLinkProgram(pa);
    GLint status=1;glGetProgramiv(pa,GL_LINK_STATUS,&status);check(!status,"failed relink reported");draw(51,153,204,"failed relink preserves installed selection");
    glAttachShader(pa,fs0);glLinkProgram(pa);glGetProgramiv(pa,GL_LINK_STATUS,&status);check(status,"relink restored");
    la=glGetUniformLocation(pa,"image");draw(204,51,17,"successful relink resets zero");glUniform1i(la,5);
    CGcontext cg=cgCreateContext();CGprogram direct=psgl_cg_create_program_from_memory(cg,single0_fpo,single0_fpo_size,CG_PROFILE_SCE_FP_RSX);
    check(direct!=NULL,"direct Cg fixture");
    if(direct){cgGLBindProgram(direct);draw(204,51,17,"direct Cg physical bindings");}
    glUseProgram(pa);draw(51,153,204,"GL bind restores logical selection");
    glUseProgram(0);glUniform1i(la,0);check(glGetError()==GL_INVALID_OPERATION,"no current program rejected");
    cgDestroyContext(cg);glDeleteProgram(pa);glDeleteProgram(pb);glDeleteProgram(p5);glDeleteProgram(pp);
    glDeleteShader(vs);glDeleteShader(fs0);glDeleteShader(fs5);glDeleteShader(fs2);glDeleteBuffers(1,&pos);glDeleteTextures(2,tex);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_SAMPLER_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(c);psglDestroyDevice(d);psglExit();return failures?1:0;
}
