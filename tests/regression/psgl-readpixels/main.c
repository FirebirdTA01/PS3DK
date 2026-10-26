#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <Cg/cgGL.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#include "vpshader_vpo.h"
#include "fpshader_fpo.h"
SYS_PROCESS_PARAM(1001, 0x100000);
static unsigned checks, failures;
static CGcontext cg;
static CGparameter color_parameter;
static GLuint vertex_buffer;
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_READ_FAIL %s\n", name); }
}
static void read_patch(const char *name, GLint x, GLint y, unsigned alignment,
                       uint32_t bottom, uint32_t next)
{
    unsigned char bytes[64], expected[64];
    unsigned stride = (12 + alignment - 1) & ~(alignment - 1);
    memset(bytes, 0xa5, sizeof(bytes));
    memset(expected, 0xa5, sizeof(expected));
    for (unsigned row = 0; row < 2; ++row) {
        uint32_t color = row ? next : bottom;
        for (unsigned col = 0; col < 3; ++col) {
            unsigned char *p = expected + 8 + row * stride + col * 4;
            p[0] = (unsigned char)(color >> 16);
            p[1] = (unsigned char)(color >> 8);
            p[2] = (unsigned char)color;
            p[3] = 255; /* XRGB rendering surfaces have no alpha channel. */
        }
    }
    glPixelStorei(GL_PACK_ALIGNMENT, (GLint)alignment);
    glReadPixels(x, y, 3, 2, GL_RGBA, GL_UNSIGNED_BYTE, bytes + 8);
    GLenum error = glGetError();
    int ok = error == GL_NO_ERROR && !memcmp(bytes, expected, sizeof(bytes));
    if (!ok) {
        printf("READ_DETAIL %s error=%x align=%u", name, (unsigned)error, alignment);
        for (unsigned i = 0; i < sizeof(bytes); ++i) printf(" %02x", bytes[i]);
        puts("");
    }
    check(ok, name);
}
static void rejected(GLsizei width, GLsizei height, GLenum format,
                     GLenum type, GLenum error, const char *name)
{
    unsigned char bytes[64], before[64];
    memset(bytes, 0xa5, sizeof(bytes));
    memcpy(before, bytes, sizeof(bytes));
    glReadPixels(0, 0, width, height, format, type, bytes + 8);
    check(glGetError() == error && !memcmp(bytes, before, sizeof(bytes)), name);
}
static int write_blob(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}
static int setup_draw(void)
{
    const char *vp_path = "/dev_hdd0/tmp/psgl-readpixels.vpo";
    const char *fp_path = "/dev_hdd0/tmp/psgl-readpixels.fpo";
    static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    static const float quad[18] = {-1,-1,0, 1,-1,0, 1,1,0, -1,-1,0, 1,1,0, -1,1,0};
    if (!write_blob(vp_path, vpshader_vpo, vpshader_vpo_size) ||
        !write_blob(fp_path, fpshader_fpo, fpshader_fpo_size)) return 0;
    cg = cgCreateContext();
    CGprogram vp = cgCreateProgramFromFile(cg, CG_BINARY, vp_path, CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    CGprogram fp = cgCreateProgramFromFile(cg, CG_BINARY, fp_path, CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    CGparameter matrix = cgGetNamedParameter(vp, "modelViewProj");
    color_parameter = cgGetNamedParameter(fp, "solidColor");
    if (!cg || !vp || !fp || !matrix || !color_parameter) return 0;
    cgGLLoadProgram(vp); cgGLLoadProgram(fp);
    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX); cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);
    cgGLBindProgram(vp); cgGLBindProgram(fp);
    cgGLSetMatrixParameterfr(matrix, identity);
    glGenBuffers(1, &vertex_buffer); glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, 3 * sizeof(float), (const void *)0);
    glEnableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND);
    glDisable(GL_DITHER); glDisable(GL_SCISSOR_TEST);
    return vertex_buffer != 0;
}
static void rectangle(int x, int y, int w, int h, float r, float g, float b)
{
    static const float colors[3][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1}};
    const float *color = colors[r ? 0 : g ? 1 : 2];
    (void)b;
    glViewport(x,y,w,h);
    cgGLSetParameter4fv(color_parameter, color);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
static void pixel(int x, int y, unsigned char r, unsigned char g,
                  unsigned char b, const char *name)
{
    unsigned char output[6] = {0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};
    const unsigned char expected[6] = {0xa5,r,g,b,255,0xa5};
    glReadPixels(x,y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,output+1);
    GLenum error = glGetError();
    int ok = error == GL_NO_ERROR && !memcmp(output,expected,sizeof(output));
    if (!ok) printf("PIXEL_DETAIL %s x=%d y=%d error=%x got=%02x%02x%02x%02x expected=%02x%02x%02x%02x\n",
        name,x,y,(unsigned)error,output[1],output[2],output[3],output[4],r,g,b,255);
    check(ok, name);
}
static void draw_complete(int x, int y, int w, int h,
                          unsigned char r, unsigned char g, unsigned char b,
                          int probe_x, int probe_y, const char *name)
{
    /* RPCS3 async shader compilation may skip cold-cache draws even when
       completion labels land. Match shader-readback's bounded warm-up;
       every final pixel still has to match, without tolerances. */
    unsigned tries;
    for (tries = 1; tries <= 10; ++tries) {
        unsigned char rgba[4] = {0xa5,0xa5,0xa5,0xa5};
        rectangle(x,y,w,h,r / 255.0f,g / 255.0f,b / 255.0f);
        glReadPixels(probe_x,probe_y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        if (rgba[0] == r && rgba[1] == g && rgba[2] == b && rgba[3] == 255)
            break;
        if (tries < 10) usleep(200000);
    }
    printf("PSGL_READ_DRAW %s attempts=%u\n", name, tries > 10 ? 10 : tries);
    pixel(probe_x,probe_y,r,g,b,name);
}
static void orientation(const char *name)
{
    /* Draw through GL viewport coordinates, not raw texture storage or the
       separately unaudited scissor path. The marker is one pixel at (2,1). */
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    draw_complete(0,0,8,2,255,0,0,1,0,"red draw completion");
    draw_complete(0,2,8,2,0,255,0,1,3,"green draw completion");
    draw_complete(2,1,1,1,0,0,255,2,1,"marker draw completion");
    pixel(1,0,255,0,0,name);
    pixel(1,3,0,255,0,name);
    pixel(2,1,0,0,255,name);
    pixel(2,2,0,255,0,name);
}
int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_READ_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    GLint value = -1;
    glGetIntegerv(GL_PACK_ALIGNMENT, &value);
    check(value == 4 && glGetError() == GL_NO_ERROR, "initial pack alignment");
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES, &value);
    check(value == GL_RGBA, "read format query");
    glGetIntegerv(GL_IMPLEMENTATION_COLOR_READ_TYPE_OES, &value);
    check(value == GL_UNSIGNED_BYTE, "read type query");
    for (unsigned alignment = 1; alignment <= 8; alignment *= 2) {
        glPixelStorei(GL_PACK_ALIGNMENT, (GLint)alignment);
        glGetIntegerv(GL_PACK_ALIGNMENT, &value);
        check(value == (GLint)alignment, "pack alignment query");
        glClearColor(1, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        read_patch("default red", 2, 3, alignment, 0xff0000, 0xff0000);
        glClearColor(0, 1, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        read_patch("default green after red", 2, 3, alignment, 0x00ff00, 0x00ff00);
    }

    GLuint buffer = 0, texture = 0, fbo = 0;
#define ROW16(c) c,c,c,c,c,c,c,c,c,c,c,c,c,c,c,c
    /* Explicit storage keeps this readback test independent of the separately
       investigated GCC 16 -O3 Altivec realignment miscompile of a nested
       four-row initializer. This is a fixture, not a compiler repair. */
    static const uint32_t storage[64] = {
        ROW16(0x13ff0000), ROW16(0x1300ff00),
        ROW16(0x130000ff), ROW16(0x13ffff00)
    };
#undef ROW16
    glGenBuffers(1, &buffer); glBindBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE, buffer);
    glBufferData(GL_TEXTURE_REFERENCE_BUFFER_SCE, sizeof(storage), storage, GL_STATIC_DRAW);
    const uint32_t *mapped = (const uint32_t *)glMapBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE, GL_READ_ONLY);
    check(mapped && !memcmp(mapped,storage,sizeof(storage)), "source reference storage");
    if (mapped) printf("SOURCE_ROWS %08x %08x %08x %08x\n", mapped[0],mapped[16],mapped[32],mapped[48]);
    glUnmapBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE);
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTextureReferenceSCE(GL_TEXTURE_2D, 1, 8, 4, 1, GL_RGBA, 64, 0);
    glGenFramebuffersOES(1, &fbo); glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, texture, 0);
    check(glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES, "reference framebuffer");
    /* Known top-to-bottom RSX storage rows, independent of texture upload
       conventions: lower-left GL y=0 selects storage row 3. */
    for (unsigned alignment = 1; alignment <= 8; alignment *= 2) {
        read_patch("FBO lower-left yellow blue", 1, 0, alignment, 0xffff00, 0x0000ff);
        read_patch("FBO offset blue green", 1, 1, alignment, 0x0000ff, 0x00ff00);
    }
    glClearColor(0, 0, 1, 0); glClear(GL_COLOR_BUFFER_BIT);
    read_patch("FBO blue clear", 1, 0, 8, 0x0000ff, 0x0000ff);
    glClearColor(1, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    read_patch("FBO red after blue", 1, 0, 8, 0xff0000, 0xff0000);

    int draw_ready = setup_draw();
    check(draw_ready, "draw setup");
    if (draw_ready) {
        orientation("FBO rendered lower-left orientation");
        glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
        orientation("default rendered lower-left orientation");
        glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    }

    /* Safe even on the old zero-fill stub: products are 0 or +1, never a
       negative byte count. The new contract must reject before any write. */
    rejected(-1, 0, GL_RGBA, GL_UNSIGNED_BYTE, GL_INVALID_VALUE, "negative width");
    rejected(0, -1, GL_RGBA, GL_UNSIGNED_BYTE, GL_INVALID_VALUE, "negative height");
    rejected(-1, -1, GL_RGBA, GL_UNSIGNED_BYTE, GL_INVALID_VALUE, "both negative untouched");
    rejected(3, 2, GL_RGB, GL_UNSIGNED_BYTE, GL_INVALID_OPERATION, "unsupported format untouched");
    rejected(3, 2, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, GL_INVALID_OPERATION, "unsupported type untouched");
    /* ES 2.0 sections 2.5 and 4.3.1: invalid enums differ from valid but
       unsupported pairs. Tables 3.2/3.4 define the tokens; ReadPixels
       excludes luminance formats. Every rejection must preserve output. */
    rejected(3, 2, 0xdead, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "unknown format untouched");
    rejected(3, 2, GL_RGBA, 0xdead, GL_INVALID_ENUM, "unknown type untouched");
    rejected(3, 2, GL_RGBA, GL_FLOAT, GL_INVALID_ENUM, "float type untouched");
    rejected(3, 2, GL_RGBA, GL_UNSIGNED_SHORT, GL_INVALID_ENUM, "unpacked short untouched");
    rejected(3, 2, GL_LUMINANCE, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "luminance read untouched");
    rejected(3, 2, GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "luminance alpha read untouched");
    rejected(3, 2, GL_ALPHA, GL_UNSIGNED_BYTE, GL_INVALID_OPERATION, "unsupported alpha untouched");
    rejected(3, 2, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, GL_INVALID_OPERATION, "unsupported RGB565 untouched");
    rejected(3, 2, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, GL_INVALID_OPERATION, "unsupported RGBA5551 untouched");
    rejected(3, 2, GL_RGB, GL_UNSIGNED_SHORT_4_4_4_4, GL_INVALID_OPERATION, "mismatched packed pair untouched");
    rejected(0, 2, 0xdead, GL_UNSIGNED_BYTE, GL_INVALID_ENUM, "zero width invalid format");
    rejected(3, 0, GL_RGBA, GL_FLOAT, GL_INVALID_ENUM, "zero height invalid type");
    rejected(0, 2, GL_RGBA, GL_UNSIGNED_BYTE, GL_NO_ERROR, "zero width");
    rejected(3, 0, GL_RGBA, GL_UNSIGNED_BYTE, GL_NO_ERROR, "zero height");
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 3);
    GLenum error = glGetError();
    glGetIntegerv(GL_PACK_ALIGNMENT, &value);
    check(error == GL_INVALID_VALUE && value == 4, "invalid pack preserves state");
    glPixelStorei(0xdead, 4);
    check(glGetError() == GL_INVALID_ENUM, "invalid pixel-store pname");
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, 0, 0);
    rejected(1, 1, GL_RGBA, GL_UNSIGNED_BYTE, GL_INVALID_FRAMEBUFFER_OPERATION_OES, "incomplete framebuffer untouched");
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
    glDeleteFramebuffersOES(1, &fbo);
    glDeleteTextures(1, &texture); glDeleteBuffers(1, &buffer);
    if (vertex_buffer) glDeleteBuffers(1, &vertex_buffer);
    if (cg) cgDestroyContext(cg);
    printf("PSGL_READ_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglDestroyContext(context); psglDestroyDevice(device); psglExit();
    return failures ? 1 : 0;
}
