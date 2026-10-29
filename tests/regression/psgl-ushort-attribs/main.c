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
#define INPUT 6u
#define EXPECTED 7u
#define PREFIX 5u
static unsigned checks, failures;
static GLuint colors, indices;
static unsigned char bytes[192];
static const uint16_t array_values[4] = {65535,32768,32767,0};
static const uint16_t indexed_values[4] = {32768,32767,0,65535};
static const uint16_t overlap_values[4] = {32768,32768,32768,32768};
static const uint16_t boundaries[4] = {0,32767,32768,65535};
static const float quad[24] = {-1,-1,0,1, 1,-1,0,1, 1,1,0,1,
                             -1,-1,0,1, 1,1,0,1, -1,1,0,1};
static const uint16_t short_indices[7] = {0xeeee,8,9,10,8,10,11};
static const uint32_t long_indices[7] = {0xeeeeeeee,8,9,10,8,10,11};

static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_USHORT_FAIL %s\n", name); }
}
static void clear(void)
{
    glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
}
static int pixel(int white)
{
    unsigned char got[4] = {0}, want[4] = {white?255:0,255,white?255:0,255};
    glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
    printf("PSGL_USHORT_PIXEL rgba=%02x%02x%02x%02x\n",got[0],got[1],got[2],got[3]);
    return glGetError() == GL_NO_ERROR && memcmp(got,want,4) == 0;
}
/* Literal expected values do not reuse the conversion calculation. */
static float reference(uint16_t value, int normalized)
{
    if (!normalized) return (float)value;
    switch (value) {
    case 0: return 0.0f;
    case 1: return 0.000015259021896696422f;
    case 16384: return 0.25000381475547417f;
    case 32767: return 0.49999237048905165f;
    case 32768: return 0.5000076295109483f;
    case 65535: return 1.0f;
    default: return -123456.0f; /* Fixture input is outside the reference table. */
    }
}
static void expected(unsigned size, const uint16_t values[4], int normalized)
{
    float v[4] = {0,0,0,1};
    for (unsigned c=0; c<size; ++c) v[c] = reference(values[c],normalized);
    glVertexAttrib4f(EXPECTED,v[0],v[1],v[2],v[3]);
}
static void put16(unsigned char *p, uint16_t value)
{
    p[0] = (unsigned char)(value >> 8); p[1] = (unsigned char)value;
}
static void fill(unsigned size, unsigned stride, unsigned layout)
{
    memset(bytes,0xee,sizeof(bytes));
    for (unsigned i=0; i<12; ++i) {
        const uint16_t *v = layout==2 ? overlap_values : i>=8 ? indexed_values : array_values;
        for (unsigned c=0; c<size; ++c)
            put16(bytes+PREFIX+i*stride+2*c,layout==2 ? v[c] : i<2 ? 0 : v[c]);
    }
}
static void fill_constant(unsigned size, const uint16_t values[4])
{
    memset(bytes,0xee,sizeof(bytes));
    for (unsigned i=0; i<12; ++i)
        for (unsigned c=0; c<size; ++c)
            put16(bytes+PREFIX+i*(size*2)+2*c,values[c]);
}
static int install(unsigned size, unsigned stride, int normalized, int vbo)
{
    glBindBuffer(GL_ARRAY_BUFFER,vbo ? colors : 0);
    if (vbo) glBufferData(GL_ARRAY_BUFFER,sizeof(bytes),bytes,GL_STATIC_DRAW);
    glVertexAttribPointer(INPUT,(GLint)size,GL_UNSIGNED_SHORT,normalized?GL_TRUE:GL_FALSE,
        (GLsizei)stride,vbo?(const void *)(uintptr_t)PREFIX:bytes+PREFIX);
    return glGetError() == GL_NO_ERROR;
}
static void draw(unsigned mode)
{
    if (!mode) { glDrawArrays(GL_TRIANGLES,2,6); return; }
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices);
    if (mode==1) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(short_indices),short_indices,GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,(const void *)(uintptr_t)2);
    } else {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(long_indices),long_indices,GL_STATIC_DRAW);
        glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(uintptr_t)4);
    }
}
static void refused(GLenum wanted, const char *name)
{
    GLenum error=glGetError(); check(error==wanted && pixel(0),name);
}

int main(void)
{
    PSGLinitOptions init={0}; psglInit(&init);
    PSGLdevice *dev=psglCreateDeviceAuto(0,0,0); PSGLcontext *ctx=psglCreateContext();
    check(dev && ctx,"device/context"); if (!dev || !ctx) return 1;
    psglMakeCurrent(ctx,dev); glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    glDisable(GL_DITHER); glDisable(GL_SCISSOR_TEST);
    float positions[48]={0}, warm[48];
    memcpy(positions+2*4,quad,sizeof(quad));
    memcpy(positions+8*4,quad,3*4*sizeof(float));
    memcpy(positions+11*4,quad+5*4,4*sizeof(float));
    for (unsigned i=0; i<12; ++i)
        for (unsigned c=0; c<4; ++c) warm[4*i+c]=(float)array_values[c];
    GLuint pos=0, vs=glCreateShader(GL_VERTEX_SHADER), fs=glCreateShader(GL_FRAGMENT_SHADER), p=glCreateProgram();
    glShaderBinary(1,&vs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,vertex_vpo,vertex_vpo_size);
    glShaderBinary(1,&fs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,fragment_fpo,fragment_fpo_size);
    glAttachShader(p,vs); glAttachShader(p,fs);
    glBindAttribLocation(p,POSITION,"position"); glBindAttribLocation(p,INPUT,"value");
    glBindAttribLocation(p,EXPECTED,"expected"); glLinkProgram(p); glUseProgram(p);
    GLint linked=0; glGetProgramiv(p,GL_LINK_STATUS,&linked);
    glGenBuffers(1,&pos); glBindBuffer(GL_ARRAY_BUFFER,pos);
    glBufferData(GL_ARRAY_BUFFER,sizeof(positions),positions,GL_STATIC_DRAW);
    glVertexAttribPointer(POSITION,4,GL_FLOAT,GL_FALSE,16,0);
    glEnableVertexAttribArray(POSITION); glEnableVertexAttribArray(INPUT);
    glDisableVertexAttribArray(EXPECTED);
    glGenBuffers(1,&colors); glGenBuffers(1,&indices);
    expected(4,array_values,0);
    check(linked && glGetError()==GL_NO_ERROR,"linked logical inputs and position VBO");
    glBindBuffer(GL_ARRAY_BUFFER,0); glVertexAttribPointer(INPUT,4,GL_FLOAT,GL_FALSE,16,warm);
    int ready=0;
    for (unsigned i=0; i<10 && !ready; ++i) {
        clear(); draw(0); ready=pixel(1); if (!ready) sys_timer_usleep(200000);
    }
    check(ready,"bounded shader warmup");

    for (unsigned size=1; size<=4; ++size)
        for (unsigned norm=0; norm<2; ++norm)
            for (unsigned vbo=0; vbo<2; ++vbo)
                for (unsigned layout=0; layout<3; ++layout)
                    for (unsigned mode=0; mode<3; ++mode) {
                        unsigned stride=layout==0?2*size:layout==1?11:2;
                        fill(size,stride,layout);
                        expected(size,layout==2?overlap_values:mode?indexed_values:array_values,(int)norm);
                        char name[120]; snprintf(name,sizeof(name),"size=%u norm=%u vbo=%u layout=%u draw=%u",size,norm,vbo,layout,mode);
                        int accepted=install(size,layout==0?0:stride,(int)norm,(int)vbo);
                        if (!accepted) { check(0,name); continue; }
                        clear(); draw(mode); check(pixel(1),name);
                    }

    for (unsigned norm=0; norm<2; ++norm)
        for (unsigned vbo=0; vbo<2; ++vbo)
            for (unsigned b=0; b<4; ++b) {
                uint16_t v[4]={boundaries[b],0,0,0};
                fill_constant(1,v); expected(1,v,(int)norm);
                char name[100]; snprintf(name,sizeof(name),"boundary=%u norm=%u vbo=%u",v[0],norm,vbo);
                int accepted=install(1,0,(int)norm,(int)vbo);
                if (accepted) { clear(); draw(0); }
                check(accepted && pixel(1),name);
            }

    const uint16_t lower[4]={0,1,16384,32767}, zero[4]={0,0,0,0};
    for (unsigned i=0; i<12; ++i)
        for (unsigned c=0; c<4; ++c) warm[4*i+c]=reference(lower[c],1);
    glBindBuffer(GL_ARRAY_BUFFER,0); glVertexAttribPointer(INPUT,4,GL_FLOAT,GL_FALSE,16,warm);
    expected(4,lower,1); clear(); draw(0); check(pixel(1),"normalized lower-half float reference");
    for (unsigned vbo=0; vbo<2; ++vbo)
        for (unsigned kind=0; kind<3; ++kind) {
            const uint16_t *v=kind==1?zero:lower; int norm=kind!=0;
            fill_constant(4,v); expected(4,v,norm);
            char name[100]; snprintf(name,sizeof(name),"size4 %s vbo=%u",kind==0?"unnormalized lower-half control":kind==1?"normalized zero control":"normalized lower-half RED",vbo);
            int accepted=install(4,0,norm,(int)vbo);
            if (accepted) { clear(); draw(0); }
            check(accepted && pixel(1),name);
        }

    for (unsigned norm=0; norm<2; ++norm) {
        glEnableVertexAttribArray(INPUT);
        int conversion_ready=0;
        uint16_t last[4]={0};
        for (unsigned update=0; update<4; ++update) {
            const uint16_t values[4]={65535,32768,32767,16384};
            for (unsigned c=0; c<4; ++c) last[c]=values[update];
            fill_constant(3,last); expected(3,last,(int)norm);
            int accepted=1;
            if (!update) accepted=install(3,0,(int)norm,1);
            else if (update==1) glBufferSubData(GL_ARRAY_BUFFER,0,sizeof(bytes),bytes);
            else if (update==2) glBufferData(GL_ARRAY_BUFFER,sizeof(bytes),bytes,GL_STATIC_DRAW);
            else {
                void *mapping=glMapBuffer(GL_ARRAY_BUFFER,GL_READ_WRITE);
                if (mapping) { memcpy(mapping,bytes,sizeof(bytes)); accepted=glUnmapBuffer(GL_ARRAY_BUFFER); }
                else accepted=0;
            }
            char name[90]; snprintf(name,sizeof(name),"VBO update=%u norm=%u without setter",update,norm);
            if (accepted) { clear(); draw(0); }
            int ok=accepted && pixel(1); check(ok,name);
            if (!update) conversion_ready=ok;
        }
        /* An old incorrect descriptor must not issue unsafe GPU reads in
           negative cases. Every gated case remains a recorded RED failure. */
        if (conversion_ready) {
            clear(); glBufferData(GL_ARRAY_BUFFER,PREFIX+6,bytes,GL_STATIC_DRAW);
            glDrawArrays(GL_TRIANGLES,2,6); refused(GL_INVALID_OPERATION,"short ushort VBO refuses draw");
            glBufferData(GL_ARRAY_BUFFER,sizeof(bytes),bytes,GL_STATIC_DRAW);
            clear(); void *mapped=glMapBuffer(GL_ARRAY_BUFFER,GL_READ_WRITE);
            glDrawArrays(GL_TRIANGLES,2,6); GLenum error=glGetError(); glUnmapBuffer(GL_ARRAY_BUFFER);
            check(mapped && error==GL_INVALID_OPERATION && pixel(0),"mapped ushort VBO refuses draw");
            glBindBuffer(GL_ARRAY_BUFFER,0);
            glVertexAttribPointer(INPUT,3,GL_UNSIGNED_SHORT,norm?GL_TRUE:GL_FALSE,6,0);
            clear(); glDrawArrays(GL_TRIANGLES,2,6); refused(GL_INVALID_OPERATION,"null ushort client refuses draw");
            glVertexAttribPointer(INPUT,3,GL_UNSIGNED_SHORT,norm?GL_TRUE:GL_FALSE,6,(const void *)(UINTPTR_MAX-2u));
            clear(); glDrawArrays(GL_TRIANGLES,2,6); refused(GL_INVALID_OPERATION,"ushort client span overflow refuses draw");
            glVertexAttribPointer(INPUT,3,GL_UNSIGNED_SHORT,norm?GL_TRUE:GL_FALSE,6,bytes+PREFIX);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(long_indices),long_indices,GL_STATIC_DRAW);
            clear(); glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(uintptr_t)sizeof(long_indices));
            refused(GL_INVALID_OPERATION,"short EBO refuses draw");
            clear(); glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(UINTPTR_MAX-3u));
            refused(GL_INVALID_OPERATION,"EBO offset overflow refuses draw");
            clear(); mapped=glMapBuffer(GL_ELEMENT_ARRAY_BUFFER,GL_READ_WRITE);
            glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,(const void *)(uintptr_t)4);
            error=glGetError(); glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
            check(mapped && error==GL_INVALID_OPERATION && pixel(0),"mapped EBO refuses draw");
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,0); clear();
            glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_SHORT,short_indices+1);
            refused(GL_INVALID_OPERATION,"client indices remain unsupported");
            uint32_t huge[6]={UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX,UINT32_MAX};
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,indices); glBufferData(GL_ELEMENT_ARRAY_BUFFER,sizeof(huge),huge,GL_STATIC_DRAW);
            clear(); glDrawElements(GL_TRIANGLES,6,GL_UNSIGNED_INT,0);
            refused(GL_OUT_OF_MEMORY,"indexed extent overflow refuses draw");
            clear(); draw(2); check(pixel(1),"valid indexed ushort recovery");
        } else {
            static const char *names[]={"short ushort VBO refuses draw","mapped ushort VBO refuses draw",
                "null ushort client refuses draw","ushort client span overflow refuses draw","short EBO refuses draw",
                "EBO offset overflow refuses draw","mapped EBO refuses draw","client indices remain unsupported",
                "indexed extent overflow refuses draw","valid indexed ushort recovery"};
            for (unsigned i=0; i<sizeof(names)/sizeof(names[0]); ++i) check(0,names[i]);
        }
        glDisableVertexAttribArray(INPUT); glVertexAttrib4f(INPUT,0.25f,0.5f,0.75f,1);
        glVertexAttrib4f(EXPECTED,0.25f,0.5f,0.75f,1);
        clear(); draw(0); check(pixel(1),"disabled ushort array uses current float value");
    }
    glUseProgram(0); glDeleteProgram(p); glDeleteShader(vs); glDeleteShader(fs);
    glDeleteBuffers(1,&pos); glDeleteBuffers(1,&colors); glDeleteBuffers(1,&indices);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_USHORT_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(ctx); psglDestroyDevice(dev); psglExit(); return failures?1:0;
}
