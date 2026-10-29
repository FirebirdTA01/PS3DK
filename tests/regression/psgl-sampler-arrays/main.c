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
#include "ordered_fpo.h"
#include "reverse_fpo.h"
#include "sparse_fpo.h"
#include "trailing_fpo.h"
#include "reserved_fpo.h"
#include "duplicate_fpo.h"
#include "oversized_fpo.h"
#include "high_fpo.h"
#include "long_fpo.h"
#include "scalar_fpo.h"
#include "one_fpo.h"
#include "groups_fpo.h"
#include "cube_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
/* Absence in the unchanged archive is an explicit RED, not an API substitute. */
extern void glUniform1iv(GLint, GLsizei, const GLint *) __attribute__((weak));
extern CGprogram psgl_cg_create_program_from_memory(CGcontext, const void *, uint32_t, CGprofile);
static unsigned checks, failures;
static const float quad[24] = {-1,-1,0,1, 1,-1,0,1, 1,1,0,1, -1,-1,0,1, 1,1,0,1, -1,1,0,1};
static const unsigned char pixels[8][4] = {
    {16,32,48,255}, {40,56,72,255}, {64,80,96,255}, {88,104,120,255},
    {112,128,144,255}, {136,152,168,255}, {160,176,192,255}, {184,200,216,255}
};
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_SAMPLER_ARRAY_FAIL %s\n", name); }
}
static GLuint shader(GLenum stage, const void *bytes, unsigned size)
{
    GLuint s = glCreateShader(stage);
    glShaderBinary(1, &s, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, bytes, size);
    check(glGetError() == GL_NO_ERROR, "load genuine compiled shader");
    return s;
}
static GLuint program(GLuint vs, GLuint fs, int want)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glBindAttribLocation(p, 0, "position"); glLinkProgram(p);
    GLint linked = 0; glGetProgramiv(p, GL_LINK_STATUS, &linked);
    check(!!linked == want && glGetError() == GL_NO_ERROR, want ? "link" : "refuse ambiguous or oversized reflection");
    return p;
}
static void set(GLint location, GLsizei count, const GLint *values, GLenum error, const char *name)
{
    if (glUniform1iv) glUniform1iv(location, count, values);
    check(glUniform1iv && glGetError() == error, name);
}
static void query(GLuint p, GLint location, GLint want, GLenum error, const char *name)
{
    GLint out[3] = {123456, -99, 654321};
    glGetUniformiv(p, location, out + 1);
    check(glGetError() == error && out[0] == 123456 && out[2] == 654321 &&
          out[1] == (error == GL_NO_ERROR ? want : -99), name);
}
/* Red/blue come from the first element, green from the second. Reversing
   element routing changes the result; both textures have distinct channels. */
static void draw(unsigned first, unsigned second, unsigned blue, const char *name)
{
    unsigned char got[4] = {0}, want[4] = {pixels[first][0], pixels[second][1], pixels[blue][2], 255};
    glClearColor(0.9f,0.1f,0.8f,1); glClear(GL_COLOR_BUFFER_BIT);
    unsigned n;
    for (n = 0; n < 5; ++n) {
        glDrawArrays(GL_TRIANGLES, 0, 6); glReadPixels(4,4,1,1,GL_RGBA,GL_UNSIGNED_BYTE,got);
        if (!memcmp(got, want, 4)) break;
        usleep(100000);
    }
    printf("PSGL_SAMPLER_ARRAY_DRAW %s rgba=%02x%02x%02x%02x\n", name, got[0],got[1],got[2],got[3]);
    check(!memcmp(got, want, 4) && glGetError() == GL_NO_ERROR, name);
}
static void pair_locations(GLuint p, GLint loc[2])
{
    /* Deliberately allocate locations in the opposite order. */
    loc[1] = glGetUniformLocation(p, "a[1]"); loc[0] = glGetUniformLocation(p, "a[0]");
    check(loc[0] >= 0 && loc[1] >= 0 && loc[0] != loc[1], "distinct reversed-query locations");
    check(glGetUniformLocation(p, "a") == loc[0], "base aliases element zero");
    check(glGetUniformLocation(p, "a[2]") == -1 && glGetUniformLocation(p, "a[-1]") == -1 &&
          glGetUniformLocation(p, "a[01]") == -1 && glGetUniformLocation(p, "a[1]x") == -1,
          "out of range and noncanonical names absent");
}
int main(void)
{
    PSGLinitOptions init = {0}; psglInit(&init);
    PSGLdevice *d = psglCreateDeviceAuto(0,0,0); PSGLcontext *c = psglCreateContext();
    if (!d || !c) { puts("PSGL_SAMPLER_ARRAY_FAIL setup"); return 1; }
    psglMakeCurrent(c,d); glViewport(0,0,8,8);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_DITHER);
    check(glUniform1iv != NULL, "glUniform1iv entry point exists");
    GLuint vs = shader(GL_VERTEX_SHADER, vertex_vpo, vertex_vpo_size);
    GLuint fs[] = {
        shader(GL_FRAGMENT_SHADER, ordered_fpo, ordered_fpo_size),
        shader(GL_FRAGMENT_SHADER, reverse_fpo, reverse_fpo_size),
        shader(GL_FRAGMENT_SHADER, sparse_fpo, sparse_fpo_size),
        shader(GL_FRAGMENT_SHADER, trailing_fpo, trailing_fpo_size),
        shader(GL_FRAGMENT_SHADER, reserved_fpo, reserved_fpo_size),
        shader(GL_FRAGMENT_SHADER, scalar_fpo, scalar_fpo_size),
        shader(GL_FRAGMENT_SHADER, cube_fpo, cube_fpo_size),
        shader(GL_FRAGMENT_SHADER, one_fpo, one_fpo_size),
        shader(GL_FRAGMENT_SHADER, groups_fpo, groups_fpo_size),
        shader(GL_FRAGMENT_SHADER, duplicate_fpo, duplicate_fpo_size),
        shader(GL_FRAGMENT_SHADER, oversized_fpo, oversized_fpo_size),
        shader(GL_FRAGMENT_SHADER, high_fpo, high_fpo_size),
        shader(GL_FRAGMENT_SHADER, long_fpo, long_fpo_size)
    };
    GLuint p[13];
    for (unsigned i = 0; i < 13; ++i) p[i] = program(vs, fs[i], i < 9);
    GLuint peer = program(vs, fs[0], 1);
    GLuint pos, tex[8]; glGenBuffers(1,&pos); glBindBuffer(GL_ARRAY_BUFFER,pos);
    glBufferData(GL_ARRAY_BUFFER,sizeof(quad),quad,GL_STATIC_DRAW);
    glUseProgram(p[0]); glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,0); glEnableVertexAttribArray(0);
    glGenTextures(8,tex);
    for (unsigned i = 0; i < 8; ++i) {
        glActiveTexture(GL_TEXTURE0+i); glBindTexture(GL_TEXTURE_2D,tex[i]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,pixels[i]);
    }
    GLint loc[2], other[2]; pair_locations(p[0],loc); pair_locations(peer,other);
    query(p[0],loc[0],0,GL_NO_ERROR,"default zero first"); query(p[0],loc[1],0,GL_NO_ERROR,"default zero second");
    draw(0,0,0,"default pixels");
    for (GLint i = 0; i < 8; ++i) {
        GLint units[2] = {i,7-i};
        set(loc[0],2,units,GL_NO_ERROR,"all units vector setter");
        query(p[0],loc[0],i,GL_NO_ERROR,"all units first getter");
        query(p[0],loc[1],7-i,GL_NO_ERROR,"all units second getter");
        draw(i,7-i,i,"all units pixels");
    }
    GLint units[4] = {2,5,99,-1};
    set(loc[0],4,units,GL_NO_ERROR,"excess elements ignored before validation"); draw(2,5,2,"excess pixels");
    GLint last[2] = {6,99};
    set(loc[1],2,last,GL_NO_ERROR,"tail excess ignored"); draw(2,6,2,"tail pixels");
    /* Count is huge but storage holds exactly the effective span. */
    GLint tail = 4;
    set(loc[1],0x7fffffff,&tail,GL_NO_ERROR,"clamp before input reads"); draw(2,4,2,"clamped pixels");
    set(loc[0],1,&tail,GL_NO_ERROR,"partial first"); draw(4,4,4,"same logical unit alias");
    glUniform1i(loc[1],7); query(p[0],loc[1],7,GL_NO_ERROR,"scalar setter shares element state"); draw(4,7,4,"scalar setter pixels");
    GLint invalid[][2] = {{-1,1},{8,1},{1,-1},{1,8}};
    for (unsigned i = 0; i < 4; ++i) {
        set(loc[0],2,invalid[i],GL_INVALID_VALUE,"invalid first or later unit");
        query(p[0],loc[0],4,GL_NO_ERROR,"atomic first unchanged"); query(p[0],loc[1],7,GL_NO_ERROR,"atomic second unchanged");
        draw(4,7,4,"atomic pixels unchanged");
    }
    set(loc[0],-1,NULL,GL_INVALID_VALUE,"negative count");
    set(-1,-1,NULL,GL_INVALID_VALUE,"negative count even at ignored location");
    set(loc[0],0,NULL,GL_NO_ERROR,"zero count no pointer read");
    set(-1,2,NULL,GL_NO_ERROR,"minus one ignored");
    set(loc[0],1,NULL,GL_INVALID_VALUE,"null guard");
    set(0,1,&tail,GL_INVALID_OPERATION,"invalid location");
    set((GLint)((p[0]<<8)|255),1,&tail,GL_INVALID_OPERATION,"unallocated location");
    set(other[0],1,&tail,GL_INVALID_OPERATION,"foreign location");
    draw(4,7,4,"error calls leave installed pixels");
    glUseProgram(peer); draw(0,0,0,"shared shader peer defaults");
    set(other[0],2,units,GL_NO_ERROR,"peer vector"); draw(2,5,2,"peer pixels");
    query(p[0],loc[0],4,GL_NO_ERROR,"inactive program first retained");
    query(p[0],loc[1],7,GL_NO_ERROR,"inactive program second retained");
    for (unsigned i = 0; i < 3; ++i) {
        glUseProgram(p[0]); draw(4,7,4,"repeated original switch");
        glUseProgram(peer); draw(2,5,2,"repeated peer switch");
    }
    glUseProgram(0); set(loc[0],1,&tail,GL_INVALID_OPERATION,"no current program");
    query(p[0],loc[0],4,GL_NO_ERROR,"getter without current program");
    glUseProgram(p[1]); GLint rev[2]; pair_locations(p[1],rev);
    set(rev[0],2,units,GL_NO_ERROR,"reverse physical assignment"); draw(2,5,2,"reverse physical pixels");
    glUseProgram(p[2]); GLint sparse[3];
    sparse[2]=glGetUniformLocation(p[2],"a[2]"); sparse[1]=glGetUniformLocation(p[2],"a[1]"); sparse[0]=glGetUniformLocation(p[2],"a");
    check(sparse[0]>=0 && sparse[1]>=0 && sparse[2]>=0,"sparse declarations include unused middle");
    GLint holes[4]={3,7,6,99};
    set(sparse[0],4,holes,GL_NO_ERROR,"sparse full update");
    for(unsigned i=0;i<3;++i) query(p[2],sparse[i],holes[i],GL_NO_ERROR,"sparse getter");
    draw(3,6,3,"sparse pixels skip unused middle");
    glUniform1i(sparse[1],1); query(p[2],sparse[1],1,GL_NO_ERROR,"unused scalar update"); draw(3,6,3,"unused update leaves pixels");
    glUseProgram(p[3]); GLint end=glGetUniformLocation(p[3],"a[2]");
    check(end>=0 && glGetUniformLocation(p[3],"a[3]")==-1,"unused trailing declaration and bound");
    set(end,2,last,GL_NO_ERROR,"unused trailing write clamps"); query(p[3],end,6,GL_NO_ERROR,"unused trailing getter");
    draw(0,0,0,"unused trailing no hardware slot");
    glUseProgram(p[4]); GLint gap[2]; pair_locations(p[4],gap);
    GLint reserved=glGetUniformLocation(p[4],"reserved"); glUniform1i(reserved,7);
    set(gap[0],2,units,GL_NO_ERROR,"noncontiguous physical slots"); draw(2,5,7,"reserved physical slot retained");
    glUseProgram(p[5]); GLint scalar=glGetUniformLocation(p[5],"image");
    set(scalar,1,&tail,GL_NO_ERROR,"scalar count one"); query(p[5],scalar,4,GL_NO_ERROR,"scalar vector getter");
    set(scalar,2,units,GL_INVALID_OPERATION,"scalar count two refused"); query(p[5],scalar,4,GL_NO_ERROR,"scalar count error atomic");
    GLint numeric=glGetUniformLocation(p[5],"offset"); check(numeric>=0,"numeric parameter exists");
    set(numeric,1,&tail,GL_INVALID_OPERATION,"numeric vector outside subset");
    glUseProgram(p[6]); GLint cube=glGetUniformLocation(p[6],"environment");
    set(cube,1,&tail,GL_INVALID_OPERATION,"cube vector outside subset");
    glUseProgram(p[7]); GLint one=glGetUniformLocation(p[7],"a[0]");
    check(one>=0 && glGetUniformLocation(p[7],"a")==one,"one-element array alias");
    set(one,2,&tail,GL_NO_ERROR,"one-element array clamps unlike scalar");
    query(p[7],one,4,GL_NO_ERROR,"one-element array getter"); draw(4,4,4,"one-element array pixels");
    glUseProgram(p[8]); GLint groups[2]; pair_locations(p[8],groups);
    GLint b1=glGetUniformLocation(p[8],"b[1]"),b0=glGetUniformLocation(p[8],"b");
    check(b0>=0 && b1>=0,"second array declarations"); glUniform1i(b1,7);
    set(groups[0],4,units,GL_NO_ERROR,"excess does not spill into next declaration");
    query(p[8],b0,0,GL_NO_ERROR,"second declaration unused element unchanged");
    query(p[8],b1,7,GL_NO_ERROR,"second declaration used element unchanged"); draw(2,5,7,"two independent array groups");
    glUseProgram(p[0]); glDetachShader(p[0],fs[0]); glLinkProgram(p[0]);
    GLint linked=1; glGetProgramiv(p[0],GL_LINK_STATUS,&linked); check(!linked,"failed relink");
    draw(4,7,4,"failed relink retains installed arrays");
    query(p[0],loc[0],0,GL_INVALID_OPERATION,"failed relink query refuses");
    glAttachShader(p[0],fs[0]); glLinkProgram(p[0]); glGetProgramiv(p[0],GL_LINK_STATUS,&linked); check(linked,"relink recovery");
    pair_locations(p[0],loc); query(p[0],loc[0],0,GL_NO_ERROR,"relink resets first"); query(p[0],loc[1],0,GL_NO_ERROR,"relink resets second");
    draw(0,0,0,"successful relink resets pixels"); query(peer,other[1],5,GL_NO_ERROR,"relink does not reset shared peer");
    set(loc[0],2,units,GL_NO_ERROR,"restore array vector");
    CGcontext cg=cgCreateContext(); CGprogram direct=psgl_cg_create_program_from_memory(cg,ordered_fpo,ordered_fpo_size,CG_PROFILE_SCE_FP_RSX);
    check(direct!=NULL,"direct Cg array fixture");
    if(direct) { cgGLBindProgram(direct); draw(0,1,0,"direct Cg physical identity"); }
    GLint changed[2]={7,6}; set(loc[0],2,changed,GL_NO_ERROR,"GL state update during direct bind");
    draw(0,1,0,"GL setter does not change direct bindings");
    query(p[0],loc[0],7,GL_NO_ERROR,"direct bind preserves program getter");
    glUseProgram(p[0]); draw(7,6,7,"GL bind restores array selections");
    glUseProgram(0); cgDestroyContext(cg);
    glDeleteProgram(peer); for(unsigned i=0;i<13;++i){glDeleteProgram(p[i]);glDeleteShader(fs[i]);}
    glDeleteShader(vs); glDeleteBuffers(1,&pos); glDeleteTextures(8,tex);
    check(glGetError()==GL_NO_ERROR,"cleanup");
    printf("PSGL_SAMPLER_ARRAY_%s checks=%u failures=%u\n",failures?"FAIL":"OK",checks,failures);
    psglDestroyContext(c); psglDestroyDevice(d); psglExit(); return failures?1:0;
}
