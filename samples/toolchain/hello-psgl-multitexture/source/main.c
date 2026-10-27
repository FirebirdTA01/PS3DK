#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <ppu-lv2.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "../../psgl-common/psgl_sample_quit.h"

#include "shaders_bin.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define NAME "hello-psgl-multitexture"
#define SHADER_LIBRARY_PATH "/dev_hdd0/tmp/hello-psgl-multitexture.bin"
#define TEX_SIZE 32

/* Six quads in a 3 x 2 grid.  Quads 0-4 combine a base texture on unit 0
   (MODULATE with a white primary colour, so the base shows unchanged) with
   a checker on unit 1 in one texture function each: MODULATE, DECAL, ADD,
   BLEND and REPLACE.  Quad 5 enables all four fixed-function units.
   Units 1-3 need programs from a shader library built with
   rsx-cg-compiler --psgl-ffp-library (state.txt lists the unit sets). */
enum { QUAD_COUNT = 6, QUAD_VERTICES = 6 };

typedef struct Vertex {
    float position[3];
    float texcoord[2];
} Vertex;

static Vertex g_vertices[QUAD_COUNT * QUAD_VERTICES];
static uint8_t g_pixels[TEX_SIZE * TEX_SIZE * 4];

static void build_quads(void)
{
    for (int q = 0; q < QUAD_COUNT; q++) {
        const float x0 = -0.92f + (float)(q % 3) * 0.63f;
        const float y0 = (q < 3) ? 0.08f : -0.86f;
        const float x1 = x0 + 0.55f;
        const float y1 = y0 + 0.78f;
        const Vertex v[QUAD_VERTICES] = {
            {{x0, y0, 0.0f}, {0.0f, 1.0f}},
            {{x1, y0, 0.0f}, {1.0f, 1.0f}},
            {{x0, y1, 0.0f}, {0.0f, 0.0f}},
            {{x0, y1, 0.0f}, {0.0f, 0.0f}},
            {{x1, y0, 0.0f}, {1.0f, 1.0f}},
            {{x1, y1, 0.0f}, {1.0f, 0.0f}}
        };
        for (int i = 0; i < QUAD_VERTICES; i++)
            g_vertices[q * QUAD_VERTICES + i] = v[i];
    }
}

/* kind 0: opaque orange-to-blue gradient (the base).
   kind 1: checker, white squares opaque, dark squares at alpha 0x40.
   kind 2: dim green horizontal stripes for ADD.
   kind 3: grey ramp that BLEND mixes toward the env colour. */
static GLuint make_texture(int kind)
{
    GLuint texture = 0u;
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            uint8_t *p = &g_pixels[(y * TEX_SIZE + x) * 4];
            const int check = ((x / 8) + (y / 8)) & 1;
            switch (kind) {
            case 0:
                p[0] = (uint8_t)(255 - y * 6);
                p[1] = (uint8_t)(110 + x * 2);
                p[2] = (uint8_t)(40 + y * 6);
                p[3] = 0xff;
                break;
            case 1:
                p[0] = p[1] = p[2] = check ? 0xff : 0x30;
                p[3] = check ? 0xff : 0x40;
                break;
            case 2:
                p[0] = 0x00;
                p[1] = ((y / 4) & 1) ? 0x60 : 0x00;
                p[2] = 0x00;
                p[3] = 0xff;
                break;
            default:
                p[0] = p[1] = p[2] = (uint8_t)(x * 8);
                p[3] = 0xff;
                break;
            }
        }
    }
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TEX_SIZE, TEX_SIZE, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, g_pixels);
    return texture;
}

/* Binds texture to unit k with a texture function; texture 0 disables it. */
static void set_unit(int k, GLuint texture, GLint mode)
{
    glActiveTexture(GL_TEXTURE0 + k);
    if (!texture) {
        glDisable(GL_TEXTURE_2D);
        return;
    }
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, mode);
    glEnable(GL_TEXTURE_2D);
}

static int write_blob(const char *path, const unsigned char *data,
                      unsigned int size)
{
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    if (fwrite(data, 1u, size, file) != size) {
        fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

int main(void)
{
    static const GLint k_unit1_modes[5] = {
        GL_MODULATE, GL_DECAL, GL_ADD, GL_BLEND, GL_REPLACE
    };
    static const float k_env_magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    static const float k_env_yellow[4] = {1.0f, 1.0f, 0.0f, 1.0f};
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    GLuint vbo = 0u;
    GLuint textures[4];
    GLenum error;

    if (!write_blob(SHADER_LIBRARY_PATH, shaders_bin, shaders_bin_size)) {
        printf(NAME ": library write failed\n");
        return 1;
    }

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf(NAME ": PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);
    psglLoadShaderLibrary(SHADER_LIBRARY_PATH);

    for (int k = 0; k < 4; k++) textures[k] = make_texture(k);

    build_quads();
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertices), g_vertices, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void *)0);
    glEnableClientState(GL_VERTEX_ARRAY);
    /* Every unit reads the same texcoords from the buffer. */
    for (int k = 0; k < 4; k++) {
        glClientActiveTexture(GL_TEXTURE0 + k);
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex),
                          (const void *)(uintptr_t)sizeof(((Vertex *)0)->position));
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glClientActiveTexture(GL_TEXTURE0);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    psgl_sample_viewport(device);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);

    error = glGetError();
    if (error != GL_NO_ERROR) {
        printf(NAME ": setup GL error 0x%04x\n", (unsigned)error);
        psglExit();
        return 1;
    }

    printf(NAME ": ready\n");
    int exit_code = psgl_sample_quit_begin(NAME);
    for (int i = 0; i < 900 && exit_code == 0; i++) {
        int quit = psgl_sample_quit_poll(NAME);
        if (quit < 0) { exit_code = 1; break; }
        if (quit > 0) break;
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        for (int q = 0; q < 5; q++) {
            set_unit(0, textures[0], GL_MODULATE);
            set_unit(1, textures[1], k_unit1_modes[q]);
            if (k_unit1_modes[q] == GL_BLEND)
                glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, k_env_magenta);
            set_unit(2, 0u, 0);
            set_unit(3, 0u, 0);
            glDrawArrays(GL_TRIANGLES, q * QUAD_VERTICES, QUAD_VERTICES);
        }
        /* All four units: base, checker MODULATE, stripes ADD, and a ramp
           that BLEND mixes toward yellow. */
        set_unit(0, textures[0], GL_MODULATE);
        set_unit(1, textures[1], GL_MODULATE);
        set_unit(2, textures[2], GL_ADD);
        set_unit(3, textures[3], GL_BLEND);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, k_env_yellow);
        glDrawArrays(GL_TRIANGLES, 5 * QUAD_VERTICES, QUAD_VERTICES);
        if (i == 0 && (error = glGetError()) != GL_NO_ERROR) {
            /* INVALID_OPERATION here means the library was not loaded. */
            printf(NAME ": draw GL error 0x%04x\n", (unsigned)error);
            exit_code = 1;
            break;
        }
        psglSwap();
        sys_timer_usleep(16000);
    }

    psgl_sample_quit_end();
    printf(NAME ": %s\n", psgl_sample_quit_outcome(exit_code));
    psglExit();
    return exit_code;
}
