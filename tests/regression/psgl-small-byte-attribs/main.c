#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/timer.h>
#include "vertex_vpo.h"
#include "fragment_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);
#define POSITION 3u
#define COLOR 6u
#define PREFIX 5u
static unsigned checks, failures;
static GLuint colors, indices;
static unsigned char bytes[160];
static const float quad[24]={-1,-1,0,1, 1,-1,0,1, 1,1,0,1,
                            -1,-1,0,1, 1,1,0,1, -1,1,0,1};
static const uint16_t short_indices[7]={0xeeee,8,9,10,8,10,11};
static const uint32_t long_indices[7]={0xeeeeeeee,8,9,10,8,10,11};
static void check(int ok,const char *name)
{
    ++checks;
    if(!ok){++failures;printf("PSGL_SMALL_BYTE_FAIL %s\n",name);}
}
static void clear(void)
{
    glClearColor(0,1,0,1);glClear(GL_COLOR_BUFFER_BIT);
}
static int pixel(unsigned char r,unsigned char g,unsigned char b)
{
    unsigned char got[4]={0},want[4]={r,g,b,255};
    glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
    printf("PSGL_SMALL_BYTE_PIXEL rgba=%02x%02x%02x%02x\n",got[0],got[1],got[2],got[3]);
    return glGetError()==GL_NO_ERROR && memcmp(got,want,4)==0;
}
static void draw(unsigned mode)
{
    if(!mode){glDrawArrays(GL_TRIANGLES,2,6);return;}
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices);
    if(mode==1){
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(short_indices),short_indices,GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,(const void *)(uintptr_t)2);
    }else{
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(long_indices),long_indices,GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(uintptr_t)4);
    }
}
static void fill(unsigned size,unsigned stride,int overlap,unsigned char r)
{
    memset(bytes,0xee,sizeof(bytes));
    for(unsigned i=0;i<12;++i){
        /* Indexed vertices differ from the DrawArrays range, detecting a
           staged base shift that would otherwise preserve uniform colors. */
        unsigned char value[3]={(i>=8 && r==64)?192:r,128,255};
        for(unsigned c=0;c<size;++c)
            bytes[PREFIX+i*stride+c]=overlap?64:(i<2?0:value[c]);
    }
}
static int install(unsigned size,unsigned stride,int vbo)
{
    glBindBuffer(GL_ARRAY_BUFFER,vbo?colors:0);
    if(vbo)glBufferData(GL_ARRAY_BUFFER,sizeof(bytes),bytes,GL_STATIC_DRAW);
    glVertexAttribPointer(COLOR,(GLint)size,GL_UNSIGNED_BYTE,GL_FALSE,(GLsizei)stride,
        vbo?(const void *)(uintptr_t)PREFIX:bytes+PREFIX);
    return glGetError()==GL_NO_ERROR;
}
static void refused(GLenum expected,const char *name)
{
    GLenum error=glGetError();
    check(error==expected && pixel(0,255,0),name);
}
int main(void)
{
    PSGLinitOptions init={0};psglInit(&init);
    PSGLdevice *dev=psglCreateDeviceAuto(0,0,0);PSGLcontext *ctx=psglCreateContext();
    check(dev && ctx,"device/context");if(!dev||!ctx)return 1;
    psglMakeCurrent(ctx,dev);glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);
    glDisable(GL_DITHER);glDisable(GL_SCISSOR_TEST);
    float positions[48]={0},warm[48];
    memcpy(positions+2*4,quad,sizeof(quad));
    memcpy(positions+8*4,quad,3*4*sizeof(float));
    memcpy(positions+11*4,quad+5*4,4*sizeof(float));
    for(unsigned i=0;i<12;++i){warm[4*i]=64;warm[4*i+1]=128;warm[4*i+2]=255;warm[4*i+3]=1;}
    GLuint pos=0,vs=glCreateShader(GL_VERTEX_SHADER),fs=glCreateShader(GL_FRAGMENT_SHADER),p=glCreateProgram();
    glShaderBinary(1,&vs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,vertex_vpo,vertex_vpo_size);
    glShaderBinary(1,&fs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,fragment_fpo,fragment_fpo_size);
    glAttachShader(p,vs);glAttachShader(p,fs);
    glBindAttribLocation(p,POSITION,"position");glBindAttribLocation(p,COLOR,"color");
    glLinkProgram(p);glUseProgram(p);GLint linked=0;glGetProgramiv(p,GL_LINK_STATUS,&linked);
    glGenBuffers(1,&pos);glBindBuffer(GL_ARRAY_BUFFER,pos);
    glBufferData(GL_ARRAY_BUFFER,sizeof(positions),positions,GL_STATIC_DRAW);
    glVertexAttribPointer(POSITION,4,GL_FLOAT,GL_FALSE,16,0);
    glEnableVertexAttribArray(POSITION);glEnableVertexAttribArray(COLOR);
    glGenBuffers(1,&colors);glGenBuffers(1,&indices);
    check(linked && glGetError()==GL_NO_ERROR,"linked logical locations and position VBO");
    glBindBuffer(GL_ARRAY_BUFFER,0);glVertexAttribPointer(COLOR,4,GL_FLOAT,GL_FALSE,16,warm);
    int ready=0;
    for(unsigned i=0;i<10&&!ready;++i){clear();draw(0);ready=pixel(64,128,255);if(!ready)sys_timer_usleep(200000);}
    check(ready,"bounded shader warmup");

    for(unsigned size=1;size<=3;++size)
        for(unsigned vbo=0;vbo<2;++vbo)
            for(unsigned layout=0;layout<3;++layout)
                for(unsigned mode=0;mode<3;++mode){
                    unsigned stride=layout==0?size:(layout==1?7:1);
                    fill(size,stride,layout==2,64);
                    char name[100];snprintf(name,sizeof(name),"size=%u vbo=%u layout=%u draw=%u",size,vbo,layout,mode);
                    int accepted=install(size,layout==0?0:stride,(int)vbo);
                    if(!accepted){check(0,name);continue;} /* Baseline never draws a rejected descriptor. */
                    clear();draw(mode);
                    check(pixel(mode && layout!=2?192:64,size>=2?(layout==2?64:128):0,
                                size==3?(layout==2?64:255):0),name);
                }

    /* A live VBO must be read again without another attribute setter. */
    fill(3,3,0,64);int accepted=install(3,0,1);
    if(accepted){clear();draw(0);check(pixel(64,128,255),"VBO before update");}
    else check(0,"VBO before update");
    fill(3,3,0,128);glBindBuffer(GL_ARRAY_BUFFER,colors);
    glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(bytes),bytes);
    if(accepted){clear();draw(0);check(pixel(128,128,255),"VBO subdata recopied without setter");}
    else check(0,"VBO subdata recopied without setter");
    fill(3,3,0,192);glBufferData(GL_ARRAY_BUFFER,sizeof(bytes),bytes,GL_STATIC_DRAW);
    if(accepted){clear();draw(0);check(pixel(192,128,255),"VBO replacement recopied without setter");}
    else check(0,"VBO replacement recopied without setter");

    /* Each rejected draw must leave the cleared surface intact. */
    if(accepted){
        clear();glBufferData(GL_ARRAY_BUFFER,PREFIX+3,bytes,GL_STATIC_DRAW);
        glDrawArrays(GL_TRIANGLES,2,6);refused(GL_INVALID_OPERATION,"short converted VBO refuses draw");
        glBufferData(GL_ARRAY_BUFFER,sizeof(bytes),bytes,GL_STATIC_DRAW);
        clear();void *mapped=glMapBuffer(GL_ARRAY_BUFFER,GL_READ_WRITE);
        glDrawArrays(GL_TRIANGLES,2,6);GLenum error=glGetError();glUnmapBuffer(GL_ARRAY_BUFFER);
        check(mapped && error==GL_INVALID_OPERATION && pixel(0,255,0),"mapped converted VBO refuses draw");
        glBindBuffer(GL_ARRAY_BUFFER,0);
        glVertexAttribPointer(COLOR,3,GL_UNSIGNED_BYTE,GL_FALSE,3,0);
        clear();glDrawArrays(GL_TRIANGLES,2,6);refused(GL_INVALID_OPERATION,"null client source refuses draw");
        glVertexAttribPointer(COLOR,3,GL_UNSIGNED_BYTE,GL_FALSE,3,(const void *)(UINTPTR_MAX-2u));
        clear();glDrawArrays(GL_TRIANGLES,2,6);refused(GL_INVALID_OPERATION,"client span overflow refuses draw");
        glVertexAttribPointer(COLOR,3,GL_UNSIGNED_BYTE,GL_FALSE,3,bytes+PREFIX);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(long_indices),long_indices,GL_STATIC_DRAW);
        clear();glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(uintptr_t)sizeof(long_indices));
        refused(GL_INVALID_OPERATION,"short EBO refuses draw");
        clear();glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(UINTPTR_MAX-3u));
        refused(GL_INVALID_OPERATION,"EBO offset overflow refuses draw");
        clear();mapped=glMapBuffer(GL_ELEMENT_ARRAY_BUFFER,GL_READ_WRITE);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(uintptr_t)4);
        error=glGetError();glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        check(mapped && error==GL_INVALID_OPERATION && pixel(0,255,0),"mapped EBO refuses draw");
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0);clear();
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,short_indices+1);
        refused(GL_INVALID_OPERATION,"client indices remain unsupported");
        uint32_t huge[6]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(huge),huge,GL_STATIC_DRAW);
        clear();glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
        refused(GL_OUT_OF_MEMORY,"indexed vertex extent overflow refuses draw");
        clear();draw(2);check(pixel(192,128,255),"valid indexed draw after rejected inputs");
    }else{
        static const char *names[]={"short converted VBO refuses draw","mapped converted VBO refuses draw",
            "null client source refuses draw","client span overflow refuses draw","short EBO refuses draw",
            "EBO offset overflow refuses draw","mapped EBO refuses draw","client indices remain unsupported",
            "indexed vertex extent overflow refuses draw","valid indexed draw after rejected inputs"};
        for(unsigned i=0;i<sizeof(names)/sizeof(names[0]);++i)check(0,names[i]);
    }
    glDisableVertexAttribArray(COLOR);glVertexAttrib4f(COLOR,64,128,255,1);
    clear();draw(0);check(pixel(64,128,255),"disabled small byte array uses current value");
    glUseProgram(0);glDeleteProgram(p);glDeleteShader(vs);glDeleteShader(fs);
    glDeleteBuffers(1,&pos);glDeleteBuffers(1,&colors);glDeleteBuffers(1,&indices);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_SMALL_BYTE_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(ctx);psglDestroyDevice(dev);psglExit();return failures?1:0;
}
