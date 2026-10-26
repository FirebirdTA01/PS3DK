/* Binary-only API contract, not ES2 conformance or a rendering test. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include "vertex_vpo.h"
#include "fragment_fpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);
/* Keep the baseline executable buildable with the published v0.26 headers. */
#ifndef GL_SHADER_COMPILER
#define GL_SHADER_COMPILER 0x8DFA
#endif
#ifndef GL_VIEWPORT
#define GL_VIEWPORT 0x0BA2
#endif
extern void glReleaseShaderCompiler(void) __attribute__((weak));
extern void glGetShaderSource(GLuint, GLsizei, GLsizei *, GLchar *) __attribute__((weak));
extern void glGetShaderPrecisionFormat(GLenum, GLenum, GLint *, GLint *) __attribute__((weak));

static int checks, failures;
#define CHECK(label, expr) do { ++checks; if (!(expr)) { ++failures; \
    printf("PSGL_BINARY_FAIL %s\n", label); } } while (0)
static void error_is(const char *label, GLenum expected)
{
    GLenum got = glGetError();
    ++checks;
    if (got != expected) {
        ++failures;
        printf("PSGL_BINARY_FAIL %s error=%04x expected=%04x\n", label, got, expected);
    }
    CHECK("error drains", glGetError() == GL_NO_ERROR);
}

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLcontext *a, *b;
    PSGLdevice *device;
    GLuint vs, fs, program;
    GLint iv[5] = {-1, -1, -1, -1, 12345};
    GLfloat fv[5] = {-1, -1, -1, -1, 12345};
    GLboolean bv[5] = {7, 7, 7, 7, 7};
    const GLint formats[] = {GL_COMPRESSED_RGB_S3TC_DXT1_EXT,
        GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT,
        GL_COMPRESSED_RGBA_S3TC_DXT5_EXT};
    const GLchar *source = "void main() {}";
    GLchar log[16] = "untouched";
    GLsizei length = 77;
    unsigned char junk[64] = {0};

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    a = psglCreateContext(); b = psglCreateContext();
    if (!device || !a || !b) { puts("PSGL_BINARY_FAIL setup"); return 1; }
    psglMakeCurrent(a, device);
    error_is("initial", GL_NO_ERROR);
    glGetIntegerv(GL_SHADER_COMPILER, iv);
    CHECK("compiler integer false", iv[0] == GL_FALSE);
    glGetBooleanv(GL_SHADER_COMPILER, bv);
    CHECK("compiler boolean false", bv[0] == GL_FALSE);
    glGetFloatv(GL_SHADER_COMPILER, fv);
    CHECK("compiler float false", fv[0] == 0.0f);
    glGetIntegerv(GL_NUM_SHADER_BINARY_FORMATS, iv);
    CHECK("one binary format", iv[0] == 1);
    glGetIntegerv(GL_SHADER_BINARY_FORMATS, iv);
    CHECK("CGB format", iv[0] == GL_SHADER_BINARY_FORMAT_CGB_PS3DK);
    glGetIntegerv(GL_NUM_COMPRESSED_TEXTURE_FORMATS, iv);
    CHECK("four DXT enums", iv[0] == 4);
    glGetIntegerv(GL_COMPRESSED_TEXTURE_FORMATS, iv);
    glGetFloatv(GL_COMPRESSED_TEXTURE_FORMATS, fv);
    glGetBooleanv(GL_COMPRESSED_TEXTURE_FORMATS, bv);
    for (unsigned i = 0; i != 4; ++i) {
        CHECK("DXT integer", iv[i] == formats[i]);
        CHECK("DXT float", fv[i] == (GLfloat)formats[i]);
        CHECK("DXT boolean", bv[i] == GL_TRUE);
    }
    CHECK("query fences", iv[4] == 12345 && fv[4] == 12345 && bv[4] == 7);
    error_is("queries", GL_NO_ERROR);

    /* Preserve the legacy fallback for valid but unaudited integer pnames.
     * A later state-query audit must return the actual viewport here. */
    iv[0] = 12345;
    glGetIntegerv(GL_VIEWPORT, iv);
    CHECK("legacy viewport fallback", iv[0] == 0);
    error_is("valid unaudited query is not invalid enum", GL_NO_ERROR);

    glShaderBinary(0, NULL, 0xffffffffu, NULL, 0);
    glCompileShader(0);
    error_is("first error retained", GL_INVALID_ENUM);
    glCompileShader(0);
    psglMakeCurrent(b, device);
    error_is("context b clean", GL_NO_ERROR);
    glShaderBinary(0, NULL, 0xffffffffu, NULL, 0);
    psglMakeCurrent(a, device);
    error_is("context a retained", GL_INVALID_OPERATION);
    psglMakeCurrent(b, device);
    error_is("context b retained", GL_INVALID_ENUM);
    psglMakeCurrent(a, device);

    vs = glCreateShader(GL_VERTEX_SHADER); fs = glCreateShader(GL_FRAGMENT_SHADER);
    program = glCreateProgram();
    CHECK("objects created", vs && fs && program);
    glShaderSource(vs, 1, &source, NULL);
    error_is("source unsupported", GL_INVALID_OPERATION);
    glCompileShader(vs);
    error_is("compile unsupported", GL_INVALID_OPERATION);
    const GLenum unsupported[] = {GL_COMPILE_STATUS, GL_INFO_LOG_LENGTH, GL_SHADER_SOURCE_LENGTH};
    for (unsigned i = 0; i != 3; ++i) {
        iv[0] = 12345;
        glGetShaderiv(vs, unsupported[i], iv);
        error_is("compiler query unsupported", GL_INVALID_OPERATION);
        CHECK("compiler query untouched", iv[0] == 12345);
    }
    glGetShaderInfoLog(vs, sizeof(log), &length, log);
    error_is("shader log unsupported", GL_INVALID_OPERATION);
    glGetProgramInfoLog(program, sizeof(log), &length, log);
    /* ES 2.0.25 section 6.1.8 p.131 applies the no-compiler restriction
     * to both log commands, but not GetProgramiv(INFO_LOG_LENGTH). */
    error_is("program log unsupported", GL_INVALID_OPERATION);
    CHECK("logs untouched", length == 77 && !strcmp(log, "untouched"));
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, iv);
    error_is("program log length remains queryable", GL_NO_ERROR);
    CHECK("release compiler exported", glReleaseShaderCompiler != NULL);
    if (glReleaseShaderCompiler) {
        glReleaseShaderCompiler(); error_is("release unsupported", GL_INVALID_OPERATION);
    }
    CHECK("source query exported", glGetShaderSource != NULL);
    if (glGetShaderSource) {
        glGetShaderSource(vs, sizeof(log), &length, log);
        error_is("source query unsupported", GL_INVALID_OPERATION);
        CHECK("source untouched", length == 77 && !strcmp(log, "untouched"));
    }
    CHECK("precision query exported", glGetShaderPrecisionFormat != NULL);
    if (glGetShaderPrecisionFormat) {
        iv[0] = iv[1] = iv[2] = 12345;
        glGetShaderPrecisionFormat(GL_VERTEX_SHADER, 0x8DF2, iv, iv + 2);
        error_is("precision unsupported", GL_INVALID_OPERATION);
        CHECK("precision untouched", iv[0] == 12345 && iv[1] == 12345 && iv[2] == 12345);
    }
    glShaderBinary(1, &vs, 0xffffffffu, vertex_vpo, vertex_vpo_size);
    error_is("bad binary enum", GL_INVALID_ENUM);
    glShaderBinary(-1, &vs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, vertex_vpo, vertex_vpo_size);
    error_is("negative count", GL_INVALID_VALUE);
    glShaderBinary(1, &vs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, vertex_vpo, -1);
    error_is("negative length", GL_INVALID_VALUE);
    GLuint missing = 0xffffffffu;
    glShaderBinary(1, &missing, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, vertex_vpo, vertex_vpo_size);
    error_is("missing shader", GL_INVALID_VALUE);
    GLuint repeated[] = {vs, vs};
    glShaderBinary(2, repeated, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, vertex_vpo, vertex_vpo_size);
    error_is("duplicate shader stage", GL_INVALID_OPERATION);
    glShaderBinary(1, &vs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, junk, sizeof(junk));
    error_is("bad binary bytes", GL_INVALID_VALUE);
    glShaderBinary(1, &vs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, vertex_vpo, 1);
    error_is("short binary", GL_INVALID_VALUE);
    glShaderBinary(1, &vs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, fragment_fpo, fragment_fpo_size);
    error_is("wrong stage", GL_INVALID_OPERATION);

    glShaderBinary(1, &vs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, vertex_vpo, vertex_vpo_size);
    error_is("load vertex", GL_NO_ERROR);
    glShaderBinary(1, &fs, GL_SHADER_BINARY_FORMAT_CGB_PS3DK, fragment_fpo, fragment_fpo_size);
    error_is("load fragment", GL_NO_ERROR);
    glCompileShader(vs);
    error_is("compile cannot damage binary", GL_INVALID_OPERATION);
    glAttachShader(program, vs); glAttachShader(program, fs);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, iv);
    CHECK("link binaries without compilation", iv[0] == GL_TRUE);
    glUseProgram(program);
    error_is("use binary program", GL_NO_ERROR);
    glUseProgram(0);
    printf("PSGL_BINARY_%s checks=%d failures=%d\n", failures ? "FAIL" : "OK", checks, failures);
    psglDestroyContext(b); psglDestroyContext(a); psglDestroyDevice(device); psglExit();
    return failures ? 1 : 0;
}
