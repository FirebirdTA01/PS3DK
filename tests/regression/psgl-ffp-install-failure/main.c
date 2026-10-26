/* A fixed-function program install that fails must not be recorded as
   installed.  The position-only fragment program gets its RSX ucode copy on
   first use; with the RSX local heap exhausted that copy fails.  The draw
   must be refused (GL_OUT_OF_MEMORY, nothing drawn), and once memory is back
   the next draw of the same state must install the program instead of
   drawing with the programs left by the previous state. */
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <rsx/mm.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
SYS_PROCESS_PARAM(1001, 0x100000);

static unsigned checks, failures;

static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_FFP_INSTALL_FAIL %s\n", name); }
}

static const float k_quad[6][3] = {{-1,-1,0},{1,-1,0},{1,1,0},{-1,-1,0},{1,1,0},{-1,1,0}};
static const float k_lo[6][2] = {{.25f,.5f},{.25f,.5f},{.25f,.5f},{.25f,.5f},{.25f,.5f},{.25f,.5f}};

static int read_rgb(int x, unsigned char out[4])
{
    memset(out, 0xa5, 4);
    glReadPixels(x, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return glGetError() == GL_NO_ERROR;
}

static int pixel_is(const unsigned char got[4], unsigned rgb)
{
    return got[0] == (unsigned char)(rgb >> 16) && got[1] == (unsigned char)(rgb >> 8) &&
           got[2] == (unsigned char)rgb && got[3] == 255;
}

static void draw_at(int x)
{
    glViewport(x, 0, 4, 4);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* RPCS3's asynchronous shader compiler may skip a draw of a program it has
   not compiled yet, so a row retries the identical draw a bounded number of
   times; the final pixel must match. */
static void row(const char *name, int x, unsigned rgb)
{
    unsigned char got[4];
    unsigned tries;
    for (tries = 1; tries <= 10; ++tries) {
        glClear(GL_COLOR_BUFFER_BIT);
        draw_at(x);
        if (read_rgb(x + 1, got) && pixel_is(got, rgb)) break;
        usleep(200000);
    }
    printf("PSGL_FFP_INSTALL_DRAW %s attempts=%u\n", name, tries > 10 ? 10 : tries);
    if (tries > 10)
        printf("PIXEL_DETAIL %s got=%02x%02x%02x%02x expected=%06x\n",
               name, got[0], got[1], got[2], got[3], rgb);
    check(tries <= 10, name);
}

/* Takes every RSX local block it can, largest first. */
#define MAX_BLOCKS 8192
static void *g_blocks[MAX_BLOCKS];
static unsigned g_block_count;

static unsigned long exhaust_rsx_heap(void)
{
    unsigned long taken = 0;
    for (uint32_t size = 256u << 20; size >= 64u; size >>= 1) {
        while (g_block_count < MAX_BLOCKS) {
            void *block = rsxMemalign(64u, size);
            if (!block) break;
            g_blocks[g_block_count++] = block;
            taken += size;
        }
    }
    return taken;
}

static void release_rsx_heap(void)
{
    while (g_block_count) rsxFree(g_blocks[--g_block_count]);
}

int main(void)
{
    static const unsigned char green[] = {0x00,0xff,0x00, 0x00,0xff,0x00};
    unsigned char got[4];
    GLuint texture = 0;
    GLenum error;
    unsigned long taken;
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_FFP_INSTALL_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_ALPHA_TEST);
    glClearColor(0, 0, 1, 0);
    glVertexPointer(3, GL_FLOAT, 0, k_quad);
    glEnableClientState(GL_VERTEX_ARRAY);
    glColor4f(1, 0, 1, 1);

    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, green);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glEnable(GL_TEXTURE_2D);
    glClientActiveTexture(GL_TEXTURE0);
    glTexCoordPointer(2, GL_FLOAT, 0, k_lo);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    while (glGetError() != GL_NO_ERROR) {}

    /* The unit-0 texturing programs are installed and drawn. */
    row("warm: unit 0 REPLACE draws the texel", 0, 0x00ff00);

    /* Untextured: the position-only programs, whose fragment ucode has no
       RSX copy yet.  Exhausting the heap makes that copy fail. */
    glDisable(GL_TEXTURE_2D);
    glClear(GL_COLOR_BUFFER_BIT);
    while (glGetError() != GL_NO_ERROR) {}
    taken = exhaust_rsx_heap();
    draw_at(8);
    error = glGetError();
    release_rsx_heap();
    printf("PSGL_FFP_INSTALL_HEAP taken=%lu blocks_limit=%u\n", taken, MAX_BLOCKS);
    check(taken > 0, "the RSX local heap was exhausted");
    check(error == GL_OUT_OF_MEMORY, "a failed program install reports GL_OUT_OF_MEMORY");
    check(read_rgb(9, got) && pixel_is(got, 0x0000ff), "a failed program install draws nothing");
    if (!pixel_is(got, 0x0000ff))
        printf("PIXEL_DETAIL failed-install got=%02x%02x%02x%02x expected=0000ff\n",
               got[0], got[1], got[2], got[3]);

    /* Memory is back: the same state must now install its programs. */
    row("after the failure the position-only programs are installed", 16, 0xff00ff);
    glEnable(GL_TEXTURE_2D);
    row("unit 0 REPLACE draws the texel again", 24, 0x00ff00);
    glDisable(GL_TEXTURE_2D);
    row("untextured draw after the texturing draw", 32, 0xff00ff);

    printf("PSGL_FFP_INSTALL_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglExit();
    return failures ? 1 : 0;
}
