/* Verify public upload paths and actual ARGB8 storage, not GPU sampling. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

typedef struct UploadCase {
    const char *name;
    GLenum format, type;
    unsigned bytes;
    unsigned char input[24];
    uint32_t expected[6];
} UploadCase;

static const UploadCase cases[] = {
    {"luminance", GL_LUMINANCE, GL_UNSIGNED_BYTE, 1,
     {0x00,0x7f,0xff,0x12,0x80,0xc3},
     {0xff000000,0xff7f7f7f,0xffffffff,0xff121212,0xff808080,0xffc3c3c3}},
    {"alpha", GL_ALPHA, GL_UNSIGNED_BYTE, 1,
     {0x00,0x7f,0xff,0x12,0x80,0xc3},
     {0x00000000,0x7f000000,0xff000000,0x12000000,0x80000000,0xc3000000}},
    {"luminance-alpha", GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE, 2,
     {0x11,0xee,0x22,0xdd,0x33,0xcc,0x44,0xbb,0x55,0xaa,0x66,0x99},
     {0xee111111,0xdd222222,0xcc333333,0xbb444444,0xaa555555,0x99666666}},
    {"rgb", GL_RGB, GL_UNSIGNED_BYTE, 3,
     {0x11,0x22,0x33,0x55,0x66,0x77,0x99,0xaa,0xbb,
      0x12,0x34,0x56,0x9a,0xbc,0xde,0x13,0x57,0x9b},
     {0xff112233,0xff556677,0xff99aabb,0xff123456,0xff9abcde,0xff13579b}},
    {"rgba", GL_RGBA, GL_UNSIGNED_BYTE, 4,
     {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,
      0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0,0x13,0x57,0x9b,0xdf},
     {0x44112233,0x88556677,0xcc99aabb,0x78123456,0xf09abcde,0xdf13579b}},
    {"rgba-8888", GL_RGBA, GL_UNSIGNED_INT_8_8_8_8, 4,
     {0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,
      0x12,0x34,0x56,0x78,0x9a,0xbc,0xde,0xf0,0x13,0x57,0x9b,0xdf},
     {0x44112233,0x88556677,0xcc99aabb,0x78123456,0xf09abcde,0xdf13579b}},
    {"rgba-8888-rev", GL_RGBA, GL_UNSIGNED_INT_8_8_8_8_REV, 4,
     {0x44,0x33,0x22,0x11,0x88,0x77,0x66,0x55,0xcc,0xbb,0xaa,0x99,
      0x78,0x56,0x34,0x12,0xf0,0xde,0xbc,0x9a,0xdf,0x9b,0x57,0x13},
     {0x44112233,0x88556677,0xcc99aabb,0x78123456,0xf09abcde,0xdf13579b}}
};

static unsigned checks, failures;

static void check_upload(const UploadCase *test, unsigned alignment)
{
    GLuint buffer = 0, texture = 0;
    uint32_t initial[64];
    unsigned char source[32], saved[32];
    unsigned stride = (3 * test->bytes + alignment - 1) & ~(alignment - 1);
    for (unsigned i = 0; i != 64; ++i) initial[i] = 0xa5a5a5a5;
    memset(source, 0xcd, sizeof(source));
    for (unsigned row = 0; row != 2; ++row)
        memcpy(source + row * stride, test->input + row * 3 * test->bytes, 3 * test->bytes);
    memcpy(saved, source, sizeof(saved));
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE, buffer);
    glBufferData(GL_TEXTURE_REFERENCE_BUFFER_SCE, sizeof(initial), initial, GL_STATIC_DRAW);
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTextureReferenceSCE(GL_TEXTURE_2D, 1, 5, 4, 1, GL_RGBA, 64, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, (GLint)alignment);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 3, 2, test->format, test->type, source);
    const uint32_t *mapped = (const uint32_t *)glMapBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE, GL_READ_ONLY);
    unsigned bad = 0;
    if (!buffer || !texture || !mapped) bad = 1;
    else for (unsigned y = 0; y != 4; ++y) {
        for (unsigned x = 0; x != 16; ++x) {
            uint32_t expected = y >= 1 && y <= 2 && x >= 1 && x <= 3
                ? test->expected[(y - 1) * 3 + x - 1] : 0xa5a5a5a5;
            if (mapped[y * 16 + x] != expected) ++bad;
        }
    }
    if (memcmp(source, saved, sizeof(source))) ++bad;
    if (mapped && !glUnmapBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE)) ++bad;
    if (glGetError() != GL_NO_ERROR) ++bad;
    ++checks;
    if (bad) {
        ++failures;
        printf("PSGL_UPLOAD_FAIL %s align=%u mismatches=%u\n", test->name, alignment, bad);
    }
    glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &buffer);
}

/* Allocation acceptance only: no claim that every logical format is a valid
 * framebuffer render target. Existing storage is expanded ARGB8. */
static void check_allocation(const UploadCase *test)
{
    GLuint texture = 0, fbo = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)test->format, 3, 2, 0,
                test->format, test->type, test->input);
    glGenFramebuffersOES(1, &fbo);
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, texture, 0);
    ++checks;
    if (!texture || !fbo || glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) != GL_FRAMEBUFFER_COMPLETE_OES) {
        ++failures;
        printf("PSGL_UPLOAD_FAIL %s allocation\n", test->name);
    }
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
    glDeleteFramebuffersOES(1, &fbo);
    glDeleteTextures(1, &texture);
}

int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_UPLOAD_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    for (unsigned i = 0; i != sizeof(cases) / sizeof(cases[0]); ++i) {
        for (unsigned alignment = 1; alignment <= 8; alignment *= 2)
            check_upload(&cases[i], alignment);
        check_allocation(&cases[i]);
    }
    printf("PSGL_UPLOAD_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglDestroyContext(context); psglDestroyDevice(device); psglExit();
    return failures ? 1 : 0;
}
