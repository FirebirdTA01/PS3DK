#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);
static unsigned checks, failures, observed_allocations;
static size_t observed_size;
static int observe;

/* Observe the real scratch request without replacing its allocator. */
void *__real_memalign(size_t alignment, size_t size);
void *__wrap_memalign(size_t alignment, size_t size)
{
    if (observe && alignment == 1024u * 1024u) {
        ++observed_allocations;
        observed_size = size;
    }
    return __real_memalign(alignment, size);
}
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_READ_FULL_FAIL %s\n", name); }
}

static void read_all(const char *name, unsigned source_height,
                     unsigned x, unsigned y, unsigned width, unsigned height,
                     const unsigned char *solid, size_t scratch_size)
{
    static const unsigned char colors[4][4] = {
        {0x11,0x22,0x33,0xff}, {0x44,0x55,0x66,0xff},
        {0x77,0x88,0x99,0xff}, {0xaa,0xbb,0xcc,0xff}
    };
    size_t stride = ((size_t)width * 4 + 7) & ~(size_t)7;
    size_t bytes = stride * height;
    unsigned char *output = malloc(bytes + 32);
    check(output != NULL, "output allocation");
    if (!output) return;
    memset(output, 0xa5, bytes + 32);
    glPixelStorei(GL_PACK_ALIGNMENT, 8);
    observed_allocations = 0; observed_size = 0; observe = 1;
    glReadPixels((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height,
                 GL_RGBA, GL_UNSIGNED_BYTE, output + 16);
    observe = 0;
    GLenum error = glGetError();
    printf("PSGL_READ_FULL_CASE %s x=%u y=%u width=%u height=%u "
           "scratch=%lu allocations=%u error=%x\n", name, x, y, width, height,
           (unsigned long)observed_size, observed_allocations, (unsigned)error);
    check(error == GL_NO_ERROR, "read error");
    check(observed_allocations == 1 && observed_size == scratch_size,
          "scratch allocation rounded to MiB");
    int guards = 1, pixels = 1, padding = 1;
    for (unsigned i = 0; i < 16; ++i)
        if (output[i] != 0xa5 || output[16 + bytes + i] != 0xa5) guards = 0;
    for (unsigned row = 0; row < height; ++row) {
        /* GL rows start at the bottom; the raw reference fixture is stored
           top-down. Separate literal expectations cover all four regions. */
        unsigned region_y = source_height - 1 - (y + row) >= 1024 ? 2 : 0;
        for (unsigned col = 0; col < width; ++col) {
            const unsigned char *want = solid ? solid :
                colors[region_y + (x + col >= 1024 ? 1 : 0)];
            const unsigned char *got = output + 16 + row * stride + col * 4;
            if (memcmp(got, want, 4)) {
                if (pixels) printf("FULL_PIXEL %s x=%u y=%u got=%02x%02x%02x%02x "
                    "want=%02x%02x%02x%02x\n", name,x+col,y+row,
                    got[0],got[1],got[2],got[3],want[0],want[1],want[2],want[3]);
                pixels = 0;
            }
        }
        for (size_t i = (size_t)width * 4; i < stride; ++i)
            if (output[16 + row * stride + i] != 0xa5) padding = 0;
    }
    check(guards, "output guards");
    check(padding, "row padding untouched");
    check(pixels, "every RGBA pixel");
    free(output);
}

static void reference_fbo(unsigned width, unsigned height, GLenum usage,
                          const char *name, size_t scratch_size)
{
    GLuint buffer = 0, texture = 0, fbo = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE, buffer);
    glBufferData(GL_TEXTURE_REFERENCE_BUFFER_SCE, (size_t)width * height * 4,
                  NULL, usage);
    /* Volatile fixture stores keep this transfer test independent of the
       separately investigated GCC16 -O3 row-initializer vectorization bug. */
    volatile uint32_t *storage = glMapBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE, GL_WRITE_ONLY);
    check(storage != NULL, "reference allocation");
    if (!storage) { glDeleteBuffers(1, &buffer); return; }
    for (unsigned row = 0; row < height; ++row)
        for (unsigned col = 0; col < width; ++col)
            storage[(size_t)row * width + col] = row < 1024 ?
                (col < 1024 ? 0x13112233u : 0x13445566u) :
                (col < 1024 ? 0x13778899u : 0x13aabbccu);
    __sync_synchronize();
    check(glUnmapBuffer(GL_TEXTURE_REFERENCE_BUFFER_SCE) == GL_TRUE, "reference unmap");
    glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
    glTextureReferenceSCE(GL_TEXTURE_2D, 1, width, height, 1, GL_RGBA, width * 4, 0);
    glGenFramebuffersOES(1, &fbo); glBindFramebufferOES(GL_FRAMEBUFFER_OES, fbo);
    glFramebufferTexture2DOES(GL_FRAMEBUFFER_OES, GL_COLOR_ATTACHMENT0_EXT,
                              GL_TEXTURE_2D, texture, 0);
    check(glCheckFramebufferStatusOES(GL_FRAMEBUFFER_OES) == GL_FRAMEBUFFER_COMPLETE_OES &&
          glGetError() == GL_NO_ERROR, "reference framebuffer");
    read_all(name, height, 0, 0, width, height, NULL, scratch_size);
    if (height == 1080) {
        /* Nonzero origin, both 1024 boundaries, and four padding bytes/row. */
        read_all(name, height, 1001, 23, 81, 1041, NULL, 1048576);
    }
    glBindFramebufferOES(GL_FRAMEBUFFER_OES, 0);
    glDeleteFramebuffersOES(1, &fbo); glDeleteTextures(1, &texture);
    glDeleteBuffers(1, &buffer);
}

int main(void)
{
    static const unsigned char red[4] = {255,0,0,255};
    static const unsigned char blue[4] = {0,0,255,255};
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_READ_FULL_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    GLuint width = 0, height = 0;
    psglGetRenderBufferDimensions(device, &width, &height);
    check(width > 0 && height > 0 && width <= 1920 && height <= 1080, "display dimensions");
    if (width && height && width <= 1920 && height <= 1080) {
        size_t pitch = ((size_t)width * 4 + 63) & ~(size_t)63;
        size_t scratch = (pitch * height + 1048575) & ~(size_t)1048575;
        glClearColor(1,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        read_all("default-red", height, 0,0,width,height,red,scratch);
        glClearColor(0,0,1,0); glClear(GL_COLOR_BUFFER_BIT);
        read_all("default-blue", height, 0,0,width,height,blue,scratch);
    }
    reference_fbo(1280,720,GL_STATIC_DRAW,"local-720p",4194304);
    reference_fbo(1920,1080,GL_STATIC_DRAW,"local-1080p",8388608);
    reference_fbo(1280,720,GL_SYSTEM_DRAW_SCE,"main-720p",4194304);
    reference_fbo(1920,1080,GL_SYSTEM_DRAW_SCE,"main-1080p",8388608);
    check(glGetError() == GL_NO_ERROR, "cleanup errors");
    printf("PSGL_READ_FULL_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglDestroyContext(context); psglDestroyDevice(device); psglExit();
    return failures ? 1 : 0;
}
