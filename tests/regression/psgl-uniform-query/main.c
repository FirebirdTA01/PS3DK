#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <Cg/cgGL.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include "vertex_vpo.h"
#include "single0_fpo.h"
#include "single5_fpo.h"
#include "pair_fpo.h"
#include "cube_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
/* The weak declaration lets the unchanged library run the RED: absence is
   an explicit failed assertion, never a substitute implementation. Other GL
   entry points pull gl_shader.o from the static archive in both arms. */
extern void glGetUniformiv(GLuint, GLint, GLint *) __attribute__((weak));
extern CGprogram psgl_cg_create_program_from_memory(CGcontext, const void *, uint32_t, CGprofile);
static unsigned checks, failures;
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_UNIFORM_QUERY_FAIL %s\n", name); }
}
static GLuint shader(GLenum type, const void *data, unsigned size)
{
    GLuint s = glCreateShader(type);
    glShaderBinary(1, &s, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, data, size);
    return s;
}
static GLuint program(GLuint vs, GLuint fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs); glLinkProgram(p);
    GLint linked = 0; glGetProgramiv(p, GL_LINK_STATUS, &linked);
    check(linked && glGetError() == GL_NO_ERROR, "fixture link");
    return p;
}
static void query(GLuint p, GLint loc, GLint want, GLenum error, const char *name)
{
    GLint values[3] = {0x12345678, -77, 0x23456789};
    if (glGetUniformiv) glGetUniformiv(p, loc, &values[1]);
    check(glGetUniformiv && glGetError() == error &&
          values[0] == 0x12345678 && values[2] == 0x23456789 &&
          values[1] == (error == GL_NO_ERROR ? want : -77), name);
}
int main(void)
{
    PSGLinitOptions init = {0}; psglInit(&init);
    PSGLdevice *d = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *c = psglCreateContext();
    if (!d || !c) { puts("PSGL_UNIFORM_QUERY_FAIL setup"); return 1; }
    psglMakeCurrent(c, d);
    check(glGetUniformiv != NULL, "query entry point exists");
    GLuint vs = shader(GL_VERTEX_SHADER, vertex_vpo, vertex_vpo_size);
    GLuint f0 = shader(GL_FRAGMENT_SHADER, single0_fpo, single0_fpo_size);
    GLuint f5 = shader(GL_FRAGMENT_SHADER, single5_fpo, single5_fpo_size);
    GLuint fp = shader(GL_FRAGMENT_SHADER, pair_fpo, pair_fpo_size);
    GLuint fc = shader(GL_FRAGMENT_SHADER, cube_fpo, cube_fpo_size);
    GLuint a = program(vs, f0), b = program(vs, f0);
    GLuint p5 = program(vs, f5), pair = program(vs, fp), cube = program(vs, fc);
    GLint la = glGetUniformLocation(a, "image"), lb = glGetUniformLocation(b, "image");
    GLint l5 = glGetUniformLocation(p5, "image");
    GLint lx = glGetUniformLocation(pair, "a"), ly = glGetUniformLocation(pair, "b");
    GLint lc = glGetUniformLocation(cube, "environment");
    check(la >= 0 && lb >= 0 && l5 >= 0 && lx >= 0 && ly >= 0 && lc >= 0, "fixture locations");
    query(a, la, 0, GL_NO_ERROR, "default zero without current program");
    query(p5, l5, 0, GL_NO_ERROR, "physical five defaults to logical zero");
    glUseProgram(a); glUniform1i(la, 5);
    query(a, la, 5, GL_NO_ERROR, "set logical five");
    query(b, lb, 0, GL_NO_ERROR, "shared shader independent default");
    glUseProgram(b); glUniform1i(lb, 7);
    query(a, la, 5, GL_NO_ERROR, "noncurrent program keeps five");
    query(b, lb, 7, GL_NO_ERROR, "shared shader second selection seven");
    glUseProgram(0);
    query(a, la, 5, GL_NO_ERROR, "query does not need current program");
    query(b, lb, 7, GL_NO_ERROR, "second program while unbound");
    glUseProgram(p5); glUniform1i(l5, 3);
    query(p5, l5, 3, GL_NO_ERROR, "logical three not physical five");
    glUniform1i(l5, 8); check(glGetError() == GL_INVALID_VALUE, "bad setter rejected");
    query(p5, l5, 3, GL_NO_ERROR, "bad setter preserves selection");
    glUseProgram(pair); glUniform1i(lx, 6); glUniform1i(ly, 2);
    query(pair, lx, 6, GL_NO_ERROR, "first sampler six");
    query(pair, ly, 2, GL_NO_ERROR, "second sampler two");
    glUniform1i(ly, 6); query(pair, ly, 6, GL_NO_ERROR, "shared logical unit");
    CGcontext cg = cgCreateContext();
    CGprogram direct = psgl_cg_create_program_from_memory(cg, single0_fpo, single0_fpo_size, CG_PROFILE_SCE_FP_RSX);
    check(direct != NULL, "direct Cg fixture");
    if (direct) cgGLBindProgram(direct);
    query(a, la, 5, GL_NO_ERROR, "direct Cg does not replace program query state");
    query(a, lb, 0, GL_INVALID_OPERATION, "foreign location untouched");
    query(a, -1, 0, GL_INVALID_OPERATION, "minus one query is not ignored");
    query(a, 0, 0, GL_INVALID_OPERATION, "zero location untouched");
    query(a, (GLint)((a << 8) | 255), 0, GL_INVALID_OPERATION, "unallocated location untouched");
    query(0, la, 0, GL_INVALID_VALUE, "zero program untouched");
    query(0x7fffff, la, 0, GL_INVALID_VALUE, "unknown program untouched");
    GLuint unlinked = glCreateProgram();
    query(unlinked, la, 0, GL_INVALID_OPERATION, "unlinked program untouched");
    query(cube, lc, 0, GL_INVALID_OPERATION, "unsupported cube query explicit refusal");
    GLint numeric = glGetUniformLocation(a, "offset");
    check(numeric >= 0, "numeric fixture location");
    query(a, numeric, 0, GL_INVALID_OPERATION, "numeric query explicit implementation limit");
    glUseProgram(a); glDetachShader(a, f0); glLinkProgram(a);
    GLint linked = 1; glGetProgramiv(a, GL_LINK_STATUS, &linked);
    check(!linked, "failed link fixture");
    query(a, la, 0, GL_INVALID_OPERATION, "failed relink rejects query");
    glAttachShader(a, f0); glLinkProgram(a);
    glGetProgramiv(a, GL_LINK_STATUS, &linked); check(linked, "successful relink fixture");
    la = glGetUniformLocation(a, "image");
    query(a, la, 0, GL_NO_ERROR, "relink resets program selection");
    query(b, lb, 7, GL_NO_ERROR, "relink leaves shared shader peer alone");
    glUseProgram(0); glDeleteProgram(a);
    query(a, la, 0, GL_INVALID_VALUE, "deleted program untouched");
    glDeleteProgram(b); glDeleteProgram(p5); glDeleteProgram(pair); glDeleteProgram(cube); glDeleteProgram(unlinked);
    glDeleteShader(vs); glDeleteShader(f0); glDeleteShader(f5); glDeleteShader(fp); glDeleteShader(fc);
    cgDestroyContext(cg);
    check(glGetError() == GL_NO_ERROR, "cleanup");
    printf("PSGL_UNIFORM_QUERY_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglDestroyContext(c); psglDestroyDevice(d); psglExit();
    return failures ? 1 : 0;
}
