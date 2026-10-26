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
/* libPSGL internal counters (not public API); weak for the RED build. */
extern unsigned psgl_fp_ucode_copies __attribute__((weak));
extern unsigned psgl_fp_ucode_ring_waits __attribute__((weak));
static unsigned checks, failures;
static CGparameter color_parameter;
static CGcontext g_cg;
static CGprogram g_fp_a;
static const char *const k_fp_path = "/dev_hdd0/tmp/psgl-fp-uniform-order.fpo";
static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_FP_ORDER_FAIL %s\n", name); }
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
    const char *vp_path = "/dev_hdd0/tmp/psgl-fp-uniform-order.vpo";
    const char *fp_path = k_fp_path;
    static const float identity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    static const float quad[18] = {-1,-1,0, 1,-1,0, 1,1,0, -1,-1,0, 1,1,0, -1,1,0};
    GLuint vertex_buffer = 0;
    if (!write_blob(vp_path, vpshader_vpo, vpshader_vpo_size) ||
        !write_blob(fp_path, fpshader_fpo, fpshader_fpo_size)) return 0;
    CGcontext cg = g_cg = cgCreateContext();
    CGprogram vp = cgCreateProgramFromFile(cg, CG_BINARY, vp_path, CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    CGprogram fp = cgCreateProgramFromFile(cg, CG_BINARY, fp_path, CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    CGparameter matrix = cgGetNamedParameter(vp, "modelViewProj");
    color_parameter = cgGetNamedParameter(fp, "solidColor");
    if (!cg || !vp || !fp || !matrix || !color_parameter) return 0;
    g_fp_a = fp;
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
static const float k_colors[4][4] = {{1,0,0,1},{0,1,0,1},{0,0,1,1},{1,1,1,1}};
static void rectangle(int x, int w, unsigned color)
{
    glViewport(x, 0, w, 4);
    cgGLSetParameter4fv(color_parameter, k_colors[color]);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}
static int pixel_is(int x, unsigned color, unsigned char *got)
{
    unsigned char rgba[4] = {0xa5,0xa5,0xa5,0xa5};
    glReadPixels(x, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    memcpy(got, rgba, 4);
    return glGetError() == GL_NO_ERROR &&
           rgba[0] == (unsigned char)(k_colors[color][0] * 255.0f) &&
           rgba[1] == (unsigned char)(k_colors[color][1] * 255.0f) &&
           rgba[2] == (unsigned char)(k_colors[color][2] * 255.0f) &&
           rgba[3] == 255;
}
static void expect(int x, unsigned color, const char *name)
{
    unsigned char got[4];
    int ok = pixel_is(x, color, got);
    if (!ok) printf("PIXEL_DETAIL %s x=%d got=%02x%02x%02x%02x expected-color=%u\n",
                    name, x, got[0], got[1], got[2], got[3], color);
    check(ok, name);
}
int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_FP_ORDER_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
    if (!setup_draw()) { puts("PSGL_FP_ORDER_FAIL setup draw"); return 1; }

    /* Warm-up: RPCS3 async shader compilation may skip cold-cache draws.
       Retry a single serialized draw until it lands, then stop retrying. */
    unsigned tries;
    unsigned char got[4];
    for (tries = 1; tries <= 10; ++tries) {
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        rectangle(0, 8, 3);
        if (pixel_is(1, 3, got)) break;
        usleep(200000);
    }
    printf("PSGL_FP_ORDER_WARMUP attempts=%u\n", tries > 10 ? 10 : tries);
    check(tries <= 10, "warm-up draw lands");

    /* Control: each draw is read back (GPU idle) before the uniform changes. */
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    rectangle(0, 8, 0);
    expect(1, 0, "serialized first draw");
    rectangle(8, 8, 1);
    expect(9, 1, "serialized second draw");
    expect(1, 0, "serialized first draw kept");

    /* Two queued draws with different values of one fragment uniform and no
       readback in between: each draw must keep the value set before it. */
    for (unsigned round = 0; round < 4; ++round) {
        unsigned a = round, b = (round + 1u) & 3u;
        char name[64];
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        rectangle(0, 8, a);
        rectangle(8, 8, b);
        snprintf(name, sizeof(name), "queued round %u first draw", round);
        expect(1, a, name);
        snprintf(name, sizeof(name), "queued round %u second draw", round);
        expect(9, b, name);
    }

    /* Four queued draws, one value each. */
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    for (unsigned i = 0; i < 4; ++i) rectangle((int)(i * 8u), 8, i);
    for (unsigned i = 0; i < 4; ++i) {
        char name[64];
        snprintf(name, sizeof(name), "queued four draw %u", i);
        expect((int)(i * 8u) + 1, i, name);
    }

    /* Dirty-only: redrawing without a change, or setting the value the
       program already holds, must not take a new ucode copy.  The counters
       are libPSGL internals; weak so this test still links without them. */
    check(&psgl_fp_ucode_copies != 0 && &psgl_fp_ucode_ring_waits != 0,
          "ucode copy counters are present");
    if (&psgl_fp_ucode_copies != 0) {
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        rectangle(0, 8, 2);
        expect(1, 2, "dirty-only: changed value draws");
        unsigned before = psgl_fp_ucode_copies;
        glViewport(8, 0, 8, 4); glDrawArrays(GL_TRIANGLES, 0, 6);
        cgGLSetParameter4fv(color_parameter, k_colors[2]);
        glViewport(16, 0, 8, 4); glDrawArrays(GL_TRIANGLES, 0, 6);
        expect(9, 2, "dirty-only: unchanged redraw");
        expect(17, 2, "dirty-only: same value set again");
        check(psgl_fp_ucode_copies == before, "dirty-only: no copy without a value change");
        rectangle(24, 8, 1);
        expect(25, 1, "dirty-only: next change draws");
        check(psgl_fp_ucode_copies == before + 1u, "dirty-only: one copy per value change");
    }

    /* Exhaustion: more uniform changes in one frame than the copy ring holds.
       6400 queued 2x2 cells, each with its own value, then one readback. */
    {
        enum { COLS = 160, ROWS = 40, CELLS = COLS * ROWS };
        static unsigned char frame[COLS * 2 * ROWS * 2 * 4];
        static const float palette[7][4] = {{1,0,0,1},{0,1,0,1},{1,1,0,1},{0,0,1,1},
                                            {1,0,1,1},{0,1,1,1},{1,1,1,1}};
        unsigned waits = &psgl_fp_ucode_ring_waits != 0 ? psgl_fp_ucode_ring_waits : 0u;
        unsigned bad = 0, first_bad = CELLS;
        glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
        for (unsigned i = 0; i < CELLS; ++i) {
            glViewport((GLint)((i % COLS) * 2u), (GLint)((i / COLS) * 2u), 2, 2);
            cgGLSetParameter4fv(color_parameter, palette[i % 7u]);
            glDrawArrays(GL_TRIANGLES, 0, 6);
        }
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        memset(frame, 0xa5, sizeof(frame));
        glReadPixels(0, 0, COLS * 2, ROWS * 2, GL_RGBA, GL_UNSIGNED_BYTE, frame);
        check(glGetError() == GL_NO_ERROR, "exhaustion readback");
        for (unsigned i = 0; i < CELLS; ++i) {
            const float *c = palette[i % 7u];
            for (unsigned dy = 0; dy < 2; ++dy)
                for (unsigned dx = 0; dx < 2; ++dx) {
                    unsigned x = (i % COLS) * 2u + dx, y = (i / COLS) * 2u + dy;
                    const unsigned char *p = frame + (y * COLS * 2u + x) * 4u;
                    if (p[0] != (unsigned char)(c[0] * 255.0f) || p[1] != (unsigned char)(c[1] * 255.0f) ||
                        p[2] != (unsigned char)(c[2] * 255.0f) || p[3] != 255) {
                        if (!bad) first_bad = i;
                        ++bad;
                    }
                }
        }
        printf("PSGL_FP_ORDER_EXHAUST cells=%u bad_pixels=%u first_bad_cell=%u ring_waits=%u\n",
               (unsigned)CELLS, bad, first_bad,
               (&psgl_fp_ucode_ring_waits != 0 ? psgl_fp_ucode_ring_waits : 0u) - waits);
        check(bad == 0, "exhaustion: every queued cell keeps its own value");
        check(&psgl_fp_ucode_ring_waits != 0 && psgl_fp_ucode_ring_waits > waits,
              "exhaustion: the copy ring wrapped at least once");
    }

    /* Dormant program: A takes a ring copy, then program B makes enough
       changes to wrap the ring twice, so A's slot is certainly reused.
       Rebinding A without a uniform change must recopy A exactly once and
       draw A's own value, not whatever B left in A's old slot. */
    check(&psgl_fp_ucode_copies != 0, "dormant: counters are present");
    if (&psgl_fp_ucode_copies != 0) {
        static const float red[4] = {1,0,0,1};
        static const float greens[2][4] = {{0,1,0,1},{0,0.5f,0,1}};
        CGprogram fp_b = cgCreateProgramFromFile(g_cg, CG_BINARY, k_fp_path,
                                                 CG_PROFILE_SCE_FP_RSX, NULL, NULL);
        CGparameter color_b = fp_b ? cgGetNamedParameter(fp_b, "solidColor") : NULL;
        check(fp_b && color_b, "dormant: second program");
        if (fp_b && color_b) {
            glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
            cgGLSetParameter4fv(color_parameter, red);
            glViewport(0, 0, 8, 4); glDrawArrays(GL_TRIANGLES, 0, 6);
            expect(1, 0, "dormant: A draws before B");
            cgGLLoadProgram(fp_b);
            cgGLBindProgram(fp_b);
            unsigned waits = psgl_fp_ucode_ring_waits, changes = 0;
            while (psgl_fp_ucode_ring_waits - waits < 2u && changes < 100000u) {
                cgGLSetParameter4fv(color_b, greens[changes & 1u]);
                glViewport(8, 0, 8, 4); glDrawArrays(GL_TRIANGLES, 0, 6);
                ++changes;
            }
            printf("PSGL_FP_ORDER_DORMANT b_changes=%u ring_waits=%u\n",
                   changes, psgl_fp_ucode_ring_waits - waits);
            check(psgl_fp_ucode_ring_waits - waits >= 2u, "dormant: B wrapped the ring twice");
            cgGLSetParameter4fv(color_b, k_colors[2]);
            glViewport(8, 0, 8, 4); glDrawArrays(GL_TRIANGLES, 0, 6);
            unsigned before = psgl_fp_ucode_copies;
            cgGLBindProgram(g_fp_a);
            glViewport(16, 0, 8, 4); glDrawArrays(GL_TRIANGLES, 0, 6);
            unsigned recopies = psgl_fp_ucode_copies - before;
            expect(17, 0, "dormant: A keeps its value after B reused its slot");
            expect(9, 2, "dormant: B's last value");
            printf("PSGL_FP_ORDER_DORMANT a_recopies=%u\n", recopies);
            check(recopies == 1u, "dormant: A is copied again exactly once");
        }
    }

    printf("PSGL_FP_ORDER_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglExit();
    return failures ? 1 : 0;
}
