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
#include "cube_fpo.h"
#include "mixed_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
/* Numeric tokens keep the exact-base fixture buildable before header support. */
#ifndef GL_TEXTURE_BINDING_CUBE_MAP
#define GL_TEXTURE_BINDING_CUBE_MAP 0x8514
#endif
#ifndef GL_MAX_CUBE_MAP_TEXTURE_SIZE
#define GL_MAX_CUBE_MAP_TEXTURE_SIZE 0x851c
#endif
extern CGprogram psgl_cg_create_program_from_memory(CGcontext, const void *, uint32_t, CGprofile);
extern void glUniform1iv(GLint, GLsizei, const GLint *);
static unsigned checks, failures;
static GLuint scalar, mixed, peer, cube, two, vbo, ebo;
static GLint direction, selection;
static int indexed;
static const float quad[] = {-1,-1,0,1, 1,-1,0,1, 1,1,0,1, -1,-1,0,1, 1,1,0,1, -1,1,0,1};
static const unsigned short indices[] = {0,1,2,3,4,5};
static const unsigned char colors[6][4] = {
    {32,64,96,255}, {128,48,80,255}, {64,160,112,255},
    {192,96,48,255}, {80,112,224,255}, {144,208,64,255}
};
static const unsigned char black[4] = {0,0,0,255}, sentinel[4] = {255,0,255,255};
static const float axes[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_CUBE_FAIL %s\n",name); }
}
static void error(GLenum want, const char *name)
{
    GLenum got=glGetError();
    if(got!=want) printf("PSGL_CUBE_ERROR %s got=%04x want=%04x\n",name,got,want);
    check(got==want,name);
}
static void query(GLenum token, GLint want, const char *name)
{
    GLint value=-99; glGetIntegerv(token,&value); check(value==want,name); error(GL_NO_ERROR,name);
}
static GLuint shader(GLenum stage,const void *data,unsigned size)
{
    GLuint s=glCreateShader(stage); glShaderBinary(1,&s,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,data,size);
    error(GL_NO_ERROR,"shader load"); return s;
}
static GLuint link_program(GLuint vs,GLuint fs)
{
    GLuint p=glCreateProgram(); glAttachShader(p,vs);glAttachShader(p,fs);
    glBindAttribLocation(p,0,"position");glLinkProgram(p);
    GLint status=0;glGetProgramiv(p,GL_LINK_STATUS,&status);check(status,"program link");return p;
}
static void select_program(GLuint p)
{
    glUseProgram(p);direction=glGetUniformLocation(p,"direction");selection=glGetUniformLocation(p,"environment");
    check(direction>=0 && selection>=0,"scalar locations");error(GL_NO_ERROR,"use program");
}
static void axis(unsigned face)
{
    glUniform3f(direction,axes[face][0],axes[face][1],axes[face][2]);error(GL_NO_ERROR,"direction");
}
static void issue(void)
{
    if(indexed)glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,0);else glDrawArrays(GL_TRIANGLES,0,6);
}
static void draw(const unsigned char want[4],GLenum expected,const char *name)
{
    unsigned char got[4]={0}; GLenum draw_error=GL_NO_ERROR;
    glClearColor(1,0,1,1);glClear(GL_COLOR_BUFFER_BIT);error(GL_NO_ERROR,"clear");
    /* Valid rows allow async emulator pipeline warmup; rejection rows issue once. */
    for(unsigned attempt=0;attempt<(expected==GL_NO_ERROR?5u:1u);++attempt){
        issue(); GLenum e=glGetError(); if(e!=GL_NO_ERROR)draw_error=e;
        glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
        if(!memcmp(got,want,4))break;
        usleep(100000);
    }
    printf("PSGL_CUBE_DRAW %s got=%02x%02x%02x%02x want=%02x%02x%02x%02x error=%04x\n",
        name,got[0],got[1],got[2],got[3],want[0],want[1],want[2],want[3],draw_error);
    check(draw_error==expected,name);check(!memcmp(got,want,4),name);error(GL_NO_ERROR,"readback");
}
static void params(GLint filter,GLint wrap)
{
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,filter);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,wrap);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,wrap);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_R,wrap);error(GL_NO_ERROR,"cube parameters");
}
static void face(unsigned f,unsigned size,const unsigned char color[4])
{
    unsigned char image[4*4*4];check(size<=4,"fixture face bound");
    for(unsigned i=0;i<size*size;++i)memcpy(image+i*4,color,4);
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,image);
    error(GL_NO_ERROR,"define face");
}
static void fill(unsigned size)
{
    static const unsigned order[6]={5,2,4,0,3,1};
    for(unsigned i=0;i<6;++i)face(order[i],size,colors[order[i]]);
}
static void scalar_query(GLuint p,GLint loc,GLint want,const char *name)
{
    GLint values[3]={123,-1,456};glGetUniformiv(p,loc,values+1);
    check(values[0]==123 && values[1]==want && values[2]==456,name);error(GL_NO_ERROR,name);
}
static void geometry(int client)
{
    glBindBuffer(GL_ARRAY_BUFFER,client?0:vbo);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,client?quad:0);
    glEnableVertexAttribArray(0);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);error(GL_NO_ERROR,"geometry");
}
static void api_and_faces(void)
{
    query(GL_MAX_CUBE_MAP_TEXTURE_SIZE,4096,"cube size limit");
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,two);glBindTexture(GL_TEXTURE_CUBE_MAP,cube);
    query(GL_TEXTURE_BINDING_2D,two,"independent 2D binding");query(GL_TEXTURE_BINDING_CUBE_MAP,cube,"cube binding");
    glBindTexture(GL_TEXTURE_2D,cube);error(GL_INVALID_OPERATION,"immutable cube target");
    glBindTexture(GL_TEXTURE_CUBE_MAP,two);error(GL_INVALID_OPERATION,"immutable 2D target");
    glBindTexture(GL_TEXTURE_CUBE_MAP_POSITIVE_X,cube);error(GL_INVALID_ENUM,"face cannot bind");
    query(GL_TEXTURE_BINDING_CUBE_MAP,cube,"invalid binds atomic");
    glActiveTexture(GL_TEXTURE7);query(GL_TEXTURE_BINDING_CUBE_MAP,0,"unit7 independent default");
    glBindTexture(GL_TEXTURE_CUBE_MAP,cube);query(GL_TEXTURE_BINDING_CUBE_MAP,cube,"unit7 cube binding");
    glActiveTexture(GL_TEXTURE0);params(GL_NEAREST,GL_CLAMP_TO_EDGE);
    for(unsigned i=0;i<6;++i){face(i,2,colors[i]);axis(i);draw(i==5?colors[i]:black,GL_NO_ERROR,"missing face completeness");}
    fill(2);
    for(unsigned i=0;i<6;++i){axis(i);draw(colors[i],GL_NO_ERROR,"six axis colors");}
    /* Cube face coordinate conventions, tested against fixed corner colors. */
    static const float corner_directions[6][3]={{1,.5f,.5f},{-1,.5f,-.5f},{-.5f,1,-.5f},{-.5f,-1,.5f},{-.5f,.5f,1},{.5f,.5f,-1}};
    unsigned char corners[16]={16,32,48,255, 64,80,96,255, 112,128,144,255, 160,176,192,255};
    for(unsigned f=0;f<6;++f){
        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,corners);error(GL_NO_ERROR,"corner image");
        for(unsigned k=0;k<4;++k){
            float v[3];memcpy(v,corner_directions[f],sizeof(v));
            /* Flip the two minor axes, retaining the major face axis. */
            unsigned horizontal=f<2?2:0,vertical=(f==2||f==3)?2:1;
            if(k&1)v[horizontal]=-v[horizontal];
            if(k&2)v[vertical]=-v[vertical];
            glUniform3f(direction,v[0],v[1],v[2]);
            draw(corners+4*k,GL_NO_ERROR,"off-axis literal corner");
        }
    }
    fill(2);axis(0);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);error(GL_NO_ERROR,"linear magnification");draw(colors[0],GL_NO_ERROR,"homogeneous linear");params(GL_NEAREST,GL_CLAMP_TO_EDGE);
    face(1,1,colors[1]);draw(black,GL_NO_ERROR,"dimension mismatch black");face(1,2,colors[1]);draw(colors[0],GL_NO_ERROR,"dimension repair");
    glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_X,0,GL_RGBA,0,0,0,GL_RGBA,GL_UNSIGNED_BYTE,0);error(GL_NO_ERROR,"zero face accepted");draw(black,GL_NO_ERROR,"zero face incomplete");face(1,2,colors[1]);
    unsigned char rgb[16]={128,48,80,128,48,80,0,0,128,48,80,128,48,80,0,0};
    glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_X,0,GL_RGB,2,2,0,GL_RGB,GL_UNSIGNED_BYTE,rgb);error(GL_NO_ERROR,"different face format accepted");draw(black,GL_NO_ERROR,"format mismatch black");face(1,2,colors[1]);
    unsigned char packed[8]={0xf0,0x0f,0xf0,0x0f,0xf0,0x0f,0xf0,0x0f};
    glTexImage2D(GL_TEXTURE_CUBE_MAP_NEGATIVE_X,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_SHORT_4_4_4_4,packed);error(GL_NO_ERROR,"different face type accepted");draw(black,GL_NO_ERROR,"type mismatch black");face(1,2,colors[1]);
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,-1,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,0);error(GL_INVALID_VALUE,"negative level");
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,1,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,0);error(GL_INVALID_OPERATION,"mips refused");
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,GL_RGBA,2,2,1,GL_RGBA,GL_UNSIGNED_BYTE,0);error(GL_INVALID_VALUE,"border refused");
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,GL_RGBA,2,1,0,GL_RGBA,GL_UNSIGNED_BYTE,0);error(GL_INVALID_VALUE,"nonsquare refused");
    glTexImage2D(GL_TEXTURE_CUBE_MAP,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_BYTE,0);error(GL_INVALID_ENUM,"aggregate upload refused");
    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,1,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,corners);error(GL_INVALID_VALUE,"subimage bound");
    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,0x7fffffff,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,corners);error(GL_INVALID_VALUE,"subimage overflow bound");
    draw(colors[0],GL_NO_ERROR,"invalid uploads preserve face");
    glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,0,0,2,2,GL_RGBA,GL_UNSIGNED_BYTE,corners);error(GL_NO_ERROR,"subimage update");
    glUniform3f(direction,1,.5f,.5f);draw(corners,GL_NO_ERROR,"subimage pixels");fill(2);axis(0);
    glCopyTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,GL_RGBA,0,0,2,2,0);error(GL_INVALID_OPERATION,"cube copy refused");
    glCompressedTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,GL_RGBA,2,2,0,16,corners);error(GL_INVALID_OPERATION,"compressed cube refused");
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X,0,GL_DEPTH_COMPONENT,2,2,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_SHORT,packed);error(GL_INVALID_OPERATION,"depth cube refused");
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_ALLOCATION_HINT_SCE,GL_TEXTURE_SWIZZLED_GPU_SCE);error(GL_INVALID_OPERATION,"swizzled cube refused");
    glTextureReferenceSCE(GL_TEXTURE_CUBE_MAP,1,2,2,1,GL_RGBA,128,0);error(GL_INVALID_OPERATION,"external cube storage refused");
    GLuint framebuffer;glGenFramebuffersOES(1,&framebuffer);glBindFramebufferOES(GL_FRAMEBUFFER_OES,framebuffer);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES,GL_COLOR_ATTACHMENT0_EXT,GL_TEXTURE_CUBE_MAP_POSITIVE_X,cube,0);error(GL_INVALID_OPERATION,"cube framebuffer attachment refused");
    glBindFramebufferOES(GL_FRAMEBUFFER_OES,0);glDeleteFramebuffersOES(1,&framebuffer);
    draw(colors[0],GL_NO_ERROR,"unsupported operations preserve image");
    /* Default object retains its own face state. New cube objects use mip filtering. */
    GLuint fresh;glGenTextures(1,&fresh);glBindTexture(GL_TEXTURE_CUBE_MAP,fresh);fill(2);
    glGenerateMipmapOES(GL_TEXTURE_CUBE_MAP);error(GL_INVALID_OPERATION,"cube mip generation refused");
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    draw(colors[0],GL_NO_ERROR,"mip refusal preserves six faces");
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST_MIPMAP_LINEAR);
    draw(black,GL_NO_ERROR,"default mip filter incomplete at 2x2");fill(1);draw(colors[0],GL_NO_ERROR,"1x1 mip default complete");
    glBindTexture(GL_TEXTURE_CUBE_MAP,0);params(GL_NEAREST,GL_CLAMP_TO_EDGE);fill(1);draw(colors[0],GL_NO_ERROR,"default cube storage");
    glBindTexture(GL_TEXTURE_CUBE_MAP,fresh);fill(3);params(GL_NEAREST,GL_CLAMP_TO_EDGE);draw(colors[0],GL_NO_ERROR,"NPOT pitched faces");
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_REPEAT);draw(black,GL_NO_ERROR,"NPOT repeat incomplete");
    glDeleteTextures(1,&fresh);error(GL_NO_ERROR,"delete cube");query(GL_TEXTURE_BINDING_CUBE_MAP,0,"delete resets binding");draw(colors[0],GL_NO_ERROR,"delete restores default cube");
    glBindTexture(GL_TEXTURE_2D,fresh);error(GL_NO_ERROR,"deleted name may acquire new target");glDeleteTextures(1,&fresh);
    glBindTexture(GL_TEXTURE_CUBE_MAP,cube);params(GL_NEAREST,GL_CLAMP_TO_EDGE);fill(2);
    /* Every unpack alignment, including padded RGB rows. */
    for(GLint alignment=1;alignment<=8;alignment*=2){
        unsigned char source[32];memset(source,0xee,sizeof(source));unsigned stride=(6u+(unsigned)alignment-1)&~((unsigned)alignment-1);
        for(unsigned y=0;y<2;++y)for(unsigned x=0;x<2;++x)memcpy(source+y*stride+x*3,colors[0],3);
        glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);
        for(unsigned f=0;f<6;++f)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGB,2,2,0,GL_RGB,GL_UNSIGNED_BYTE,source);
        error(GL_NO_ERROR,"unpack RGB upload");draw(colors[0],GL_NO_ERROR,"unpack RGB pixels");
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT,4);
    for(unsigned f=0;f<6;++f)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+f,0,GL_RGBA,2,2,0,GL_RGBA,GL_UNSIGNED_SHORT_4_4_4_4,packed);
    error(GL_NO_ERROR,"packed upload");static const unsigned char red[4]={255,0,0,255};draw(red,GL_NO_ERROR,"packed pixels");fill(2);
}
static void routing(void)
{
    GLint units[2]={7,0};glUniform1i(selection,7);error(GL_NO_ERROR,"scalar selection7");scalar_query(scalar,selection,7,"scalar getter7");draw(colors[0],GL_NO_ERROR,"cube routed from physical5 to logical7");
    glUniform1iv(selection,1,units);error(GL_NO_ERROR,"scalar vector count1");
    glUniform1iv(selection,2,units);error(GL_INVALID_OPERATION,"scalar count2 refused");
    glUniform1iv(selection,0,0);error(GL_NO_ERROR,"scalar count0");
    glUniform1iv(selection,-1,0);error(GL_INVALID_VALUE,"scalar negative count");
    glUniform1i(selection,8);error(GL_INVALID_VALUE,"scalar bad unit");scalar_query(scalar,selection,7,"bad setter atomic");
    GLint original=selection;select_program(peer);scalar_query(peer,selection,0,"peer defaults");axis(1);draw(colors[1],GL_NO_ERROR,"peer routing");
    select_program(scalar);scalar_query(scalar,selection,7,"program state retained");axis(0);draw(colors[0],GL_NO_ERROR,"switch restores cube route");
    glLinkProgram(scalar);select_program(scalar);scalar_query(scalar,selection,0,"relink resets cube selection");axis(0);draw(colors[0],GL_NO_ERROR,"relinked draw");(void)original;
    /* Deletion/reuse deliberately cleared the unit0 2D binding. Restore it
       before testing only sampler-selection conflicts and their repair. */
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,two);
    query(GL_TEXTURE_BINDING_2D,two,"mixed fixture restores 2D texture");
    select_program(mixed);GLint a=glGetUniformLocation(mixed,"a"),hole=glGetUniformLocation(mixed,"a[1]");
    check(a>=0 && hole>=0,"mixed array locations");axis(0);
    static const unsigned char combined[4]={16,80,96,255};
    for(int client=0;client<2;++client)for(indexed=0;indexed<2;++indexed){
        geometry(client);GLint allzero[3]={0,0,0};glUniform1iv(a,3,allzero);glUniform1i(selection,0);error(GL_NO_ERROR,"setters allow conflict");
        draw(sentinel,GL_INVALID_OPERATION,"mixed same unit rejected before draw");
        glUniform1i(selection,7);error(GL_NO_ERROR,"repair only cube unit");draw(combined,GL_NO_ERROR,"same geometry recovery");
        glUniform1i(hole,7);error(GL_NO_ERROR,"unused array hole selection");draw(combined,GL_NO_ERROR,"unused hole not conflicting");
        glUniform1i(glGetUniformLocation(mixed,"a[2]"),7);error(GL_NO_ERROR,"introduce used array conflict");draw(sentinel,GL_INVALID_OPERATION,"active array element conflict");
        glUniform1i(glGetUniformLocation(mixed,"a[2]"),0);draw(combined,GL_NO_ERROR,"repair only array selection");
    }
    indexed=0;geometry(0);select_program(scalar);axis(0);
    CGcontext cg=cgCreateContext();
    CGprogram direct=psgl_cg_create_program_from_memory(cg,cube_fpo,cube_fpo_size,CG_PROFILE_SCE_FP_RSX);
    check(direct!=NULL,"direct cube program");
    if(direct){
        CGparameter tex=cgGetNamedParameter(direct,"environment"),dir=cgGetNamedParameter(direct,"direction");
        cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);cgGLBindProgram(direct);
        cgGLSetParameter3f(dir,1,0,0);cgGLSetTextureParameter(tex,cube);
        check(cgGetError()==CG_NO_ERROR,"direct Cg profile and parameters");
        query(GL_ACTIVE_TEXTURE,GL_TEXTURE5,"direct Cg physical unit");
        error(GL_NO_ERROR,"direct Cg cube binding");query(GL_TEXTURE_BINDING_CUBE_MAP,cube,"direct Cg chooses cube target");
        draw(colors[0],GL_NO_ERROR,"direct Cg physical slot5");
        cgGLSetupSampler(tex,cube);error(GL_NO_ERROR,"Cg setup cube sampler");draw(colors[0],GL_NO_ERROR,"Cg setup pixels");
    }
    cgGLDisableProfile(CG_PROFILE_SCE_FP_RSX);select_program(scalar);axis(0);cgDestroyContext(cg);
}
static void queued_lifetime(void)
{
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_CUBE_MAP,cube);fill(2);params(GL_NEAREST,GL_CLAMP_TO_EDGE);
    draw(colors[0],GL_NO_ERROR,"warm queued shader");
    glClearColor(1,0,1,1);glClear(GL_COLOR_BUFFER_BIT);glViewport(0,0,8,8);issue();
    face(0,2,colors[4]);glViewport(8,0,8,8);issue();
    unsigned char left[4]={0},right[4]={0};glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,left);glReadPixels(12,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,right);
    check(!memcmp(left,colors[0],4),"queued old image survives replacement");check(!memcmp(right,colors[4],4),"queued new image visible");error(GL_NO_ERROR,"queued replacement");
    glViewport(0,0,8,8);issue();glDeleteTextures(1,&cube);glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,left);
    check(!memcmp(left,colors[4],4),"queued image survives deletion");error(GL_NO_ERROR,"queued deletion");
}
int main(void)
{
    PSGLinitOptions init={0};psglInit(&init);PSGLdevice *d=psglCreateDeviceAuto(0,0,0);PSGLcontext *c=psglCreateContext();
    if(!c||!d){puts("PSGL_CUBE_FAIL setup");return 1;}psglMakeCurrent(c,d);glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_DITHER);
    GLuint vs=shader(GL_VERTEX_SHADER,vertex_vpo,vertex_vpo_size),fs=shader(GL_FRAGMENT_SHADER,cube_fpo,cube_fpo_size),mf=shader(GL_FRAGMENT_SHADER,mixed_fpo,mixed_fpo_size);
    scalar=link_program(vs,fs);peer=link_program(vs,fs);mixed=link_program(vs,mf);
    glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glGenBuffers(1,&ebo);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,ebo);glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(indices),indices,GL_STATIC_DRAW);geometry(0);
    glGenTextures(1,&two);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,two);
    static const unsigned char flat[4]={16,80,144,255};glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,flat);
    glGenTextures(1,&cube);select_program(scalar);api_and_faces();routing();queued_lifetime();
    glUseProgram(0);glDeleteProgram(scalar);glDeleteProgram(peer);glDeleteProgram(mixed);glDeleteShader(vs);glDeleteShader(fs);glDeleteShader(mf);
    glDeleteTextures(1,&two);glDeleteBuffers(1,&vbo);glDeleteBuffers(1,&ebo);error(GL_NO_ERROR,"cleanup");
    printf("PSGL_CUBE_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(c);psglDestroyDevice(d);psglExit();return failures?1:0;
}
