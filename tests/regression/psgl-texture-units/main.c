#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include "vertex_vpo.h"
#include "combined_fpo.h"
#include "unit0_fpo.h"
#include "unit1_fpo.h"
#include "unit2_fpo.h"
#include "unit3_fpo.h"
#include "unit4_fpo.h"
#include "unit5_fpo.h"
#include "unit6_fpo.h"
#include "unit7_fpo.h"
#ifndef GL_MAX_TEXTURE_IMAGE_UNITS
#define GL_MAX_TEXTURE_IMAGE_UNITS 0x8872
#define GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS 0x8B4C
#define GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS 0x8B4D
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE_BINDING_2D
#define GL_TEXTURE_BINDING_2D 0x8069
#endif
SYS_PROCESS_PARAM(1001, 0x100000);
static unsigned checks, failures;
static const float quad[24] = {-1,-1,0,1, 1,-1,0,1, 1,1,0,1, -1,-1,0,1, 1,1,0,1, -1,1,0,1};
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_TEXUNIT_FAIL %s\n", name); }
}
static void query(GLenum name, GLint expected)
{
    GLint i = -1; GLfloat f = -1; GLboolean b = 0x55;
    glGetIntegerv(name, &i); glGetFloatv(name, &f); glGetBooleanv(name, &b);
    printf("PSGL_TEXUNIT_QUERY %x int=%d float=%g bool=%u\n", (unsigned)name, (int)i, (double)f, b);
    check(i == expected && f == expected && b == (expected != 0) && glGetError() == GL_NO_ERROR, "limit query forms");
}
static void binding(GLenum unit, GLuint texture)
{
    GLint active = -1, bound = -1;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active); glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
    printf("PSGL_TEXUNIT_BIND expected=%x/%u actual=%x/%u\n", (unsigned)unit, (unsigned)texture, (unsigned)active, (unsigned)bound);
    check(active == (GLint)unit && bound == (GLint)texture && glGetError() == GL_NO_ERROR, "active binding");
}
static GLuint program(GLuint vs, const void *binary, unsigned size)
{
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER), p = glCreateProgram();
    glShaderBinary(1, &fs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, binary, (GLsizei)size);
    glAttachShader(p, vs); glAttachShader(p, fs); glBindAttribLocation(p, 0, "position"); glLinkProgram(p);
    GLint linked = 0; glGetProgramiv(p, GL_LINK_STATUS, &linked);
    check(linked == GL_TRUE && glGetError() == GL_NO_ERROR, "program link");
    glDeleteShader(fs);
    return p;
}
static void draw(GLuint p, unsigned r, unsigned g, unsigned b, const char *name)
{
    unsigned char pixel[4] = {0}, expected[4] = {r,g,b,255};
    glUseProgram(p); glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    unsigned attempt;
    for (attempt = 1; attempt <= 10; ++attempt) {
        glDrawArrays(GL_TRIANGLES, 0, 6); glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        if (!memcmp(pixel,expected,4)) break;
        if (attempt < 10) usleep(200000);
    }
    printf("PSGL_TEXUNIT_DRAW %s attempts=%u rgba=%02x%02x%02x%02x\n",name,attempt>10?10:attempt,pixel[0],pixel[1],pixel[2],pixel[3]);
    check(!memcmp(pixel,expected,4) && glGetError() == GL_NO_ERROR, name);
}
int main(void)
{
    PSGLinitOptions init = {0}; psglInit(&init);
    PSGLdevice *dev = psglCreateDeviceAuto(0,0,0); PSGLcontext *ctx = psglCreateContext();
    if (!dev || !ctx) { puts("PSGL_TEXUNIT_FAIL setup"); return 1; }
    psglMakeCurrent(ctx,dev); glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_DITHER);
    query(GL_MAX_TEXTURE_IMAGE_UNITS,8); query(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS,8);
    query(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS,0);
    /* The legacy fixed-function query is deliberately not raised here. */
    GLint legacy = 0; glGetIntegerv(GL_MAX_TEXTURE_UNITS,&legacy); check(legacy == 4,"legacy limit unchanged");
    GLuint vs = glCreateShader(GL_VERTEX_SHADER), programs[9], textures[8], pos;
    glShaderBinary(1,&vs,GL_SHADER_BINARY_FORMAT_CGB_PS3DK,vertex_vpo,vertex_vpo_size);
    const void *binaries[8] = {unit0_fpo,unit1_fpo,unit2_fpo,unit3_fpo,unit4_fpo,unit5_fpo,unit6_fpo,unit7_fpo};
    const unsigned sizes[8] = {unit0_fpo_size,unit1_fpo_size,unit2_fpo_size,unit3_fpo_size,unit4_fpo_size,unit5_fpo_size,unit6_fpo_size,unit7_fpo_size};
    for (unsigned u=0; u<8; ++u) programs[u] = program(vs,binaries[u],sizes[u]);
    programs[8] = program(vs,combined_fpo,combined_fpo_size);
    glUseProgram(programs[0]);
    glGenBuffers(1,&pos); glBindBuffer(GL_ARRAY_BUFFER,pos); glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0); glEnableVertexAttribArray(0);
    glGenTextures(8,textures);
    for (unsigned u=0; u<8; ++u) {
        unsigned char rgba[4] = {1u<<u,255u-(1u<<u),17u*u,255};
        glActiveTexture(GL_TEXTURE0+u); glBindTexture(GL_TEXTURE_2D,textures[u]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        binding(GL_TEXTURE0+u,textures[u]);
    }
    for (unsigned u=0; u<8; ++u) {
        char name[40]; snprintf(name,sizeof(name),"unit %u samples own texture",u);
        draw(programs[u],1u<<u,255u-(1u<<u),17u*u,name);
    }
    draw(programs[8],255,223,119,"eight simultaneous samplers");
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D,textures[0]);
    draw(programs[7],1,254,0,"high unit rebind"); draw(programs[0],1,254,0,"low unit unaffected");
    glBindTexture(GL_TEXTURE_2D,textures[7]);
    glActiveTexture(GL_TEXTURE8); check(glGetError()==GL_INVALID_ENUM,"upper unit refused"); binding(GL_TEXTURE7,textures[7]);
    glActiveTexture(GL_TEXTURE0-1); check(glGetError()==GL_INVALID_ENUM,"lower unit refused"); binding(GL_TEXTURE7,textures[7]);
    draw(programs[7],128,127,119,"invalid selection preserves sampler");
    GLuint shared = 0; glGenTextures(1,&shared);
    for (unsigned u=0; u<8; ++u) { glActiveTexture(GL_TEXTURE0+u); glBindTexture(GL_TEXTURE_2D,shared); }
    glDeleteTextures(1,&shared);
    for (unsigned u=0; u<8; ++u) {
        glActiveTexture(GL_TEXTURE0+u); binding(GL_TEXTURE0+u,0);
        glBindTexture(GL_TEXTURE_2D,textures[u]);
    }
    draw(programs[8],255,223,119,"rebind after shared deletion");
    glUseProgram(0);
    for(unsigned u=0;u<9;++u) glDeleteProgram(programs[u]);
    glDeleteShader(vs); glDeleteBuffers(1,&pos); glDeleteTextures(8,textures);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_TEXUNIT_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(ctx); psglDestroyDevice(dev); psglExit(); return failures?1:0;
}
