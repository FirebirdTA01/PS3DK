/* Fixed-function texture environments on units 0-3 (OpenGL 2.1 section
   3.8.13, tables 3.21/3.22), per-unit texcoord arrays (2.8), current
   texcoords (2.7), texture matrices (2.11.2) and the image/coordinate unit
   rules for units 4-7 (2.11.2).
   Expected pixels are derived by hand from those tables; every input is
   0x00, 0xff or a pass-through value, so no row depends on rounding. */
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/process.h>
#ifndef PSGL_FFP_TEXENV_NOLIB
#include "shaders_bin.h"
#endif
SYS_PROCESS_PARAM(1001, 0x100000);

#ifndef GL_COMBINE
#define GL_COMBINE 0x8570
#endif
/* Rows unused by the no-library variant are marked unused. */
/* Weak so the RED build links against a libPSGL without these queries. */
extern void glGetTexEnviv(GLenum target, GLenum pname, GLint *params) __attribute__((weak));
extern void glGetTexEnvfv(GLenum target, GLenum pname, GLfloat *params) __attribute__((weak));
/* libPSGL internal counter of fixed-function program installs (not API). */
extern unsigned psgl_ffp_program_installs __attribute__((weak));
#ifndef GL_MAX_TEXTURE_COORDS_ARB
#define GL_MAX_TEXTURE_COORDS_ARB 0x8871
#endif

#define LIB_PATH "/dev_hdd0/tmp/psgl-ffp-texenv.bin"
static unsigned checks, failures;
static int row_x;

static void check(int ok, const char *name)
{
    ++checks;
    if (!ok) { ++failures; printf("PSGL_FFP_TEXENV_FAIL %s\n", name); }
}

static const float k_quad[6][3] = {{-1,-1,0},{1,-1,0},{1,1,0},{-1,-1,0},{1,1,0},{-1,1,0}};
/* Two texels per texture: s = 0.25 samples texel 0, s = 0.75 texel 1. */
static const float k_lo[6][2] = {{.25f,.5f},{.25f,.5f},{.25f,.5f},{.25f,.5f},{.25f,.5f},{.25f,.5f}};
static const float k_hi[6][2] = {{.75f,.5f},{.75f,.5f},{.75f,.5f},{.75f,.5f},{.75f,.5f},{.75f,.5f}};
static const unsigned char k_magenta[6][4] = {{255,0,255,255},{255,0,255,255},{255,0,255,255},
                                              {255,0,255,255},{255,0,255,255},{255,0,255,255}};
#define C(v) ((v) / 255.0f)

static GLuint tex_rgba_a, tex_rgb_b, tex_alpha_c, tex_lum_d, tex_rgba_e;
static GLuint tex_la_f, tex_rgba_half_g;

static GLuint make_texture(GLenum format, const unsigned char *texels)
{
    GLuint name = 0;
    glGenTextures(1, &name);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, 2, 1, 0, format, GL_UNSIGNED_BYTE, texels);
    return name;
}

static void make_textures(void)
{
    static const unsigned char a[] = {0x80,0xff,0x40,0xff, 0x00,0x00,0xff,0x00}; /* RGBA */
    static const unsigned char b[] = {0x00,0xff,0x00, 0xff,0x00,0xff};           /* RGB */
    static const unsigned char c[] = {0xff, 0x00};                                /* ALPHA */
    static const unsigned char d[] = {0x80, 0xff};                                /* LUMINANCE */
    static const unsigned char e[] = {0x00,0x80,0xff,0xff, 0xff,0x00,0x00,0x00}; /* RGBA, A 1/0 */
    tex_rgba_a = make_texture(GL_RGBA, a);
    tex_rgb_b = make_texture(GL_RGB, b);
    tex_alpha_c = make_texture(GL_ALPHA, c);
    tex_lum_d = make_texture(GL_LUMINANCE, d);
    tex_rgba_e = make_texture(GL_RGBA, e);
    static const unsigned char f[] = {0x80,0x00, 0xff,0xff};                    /* LUMINANCE_ALPHA */
    static const unsigned char g[] = {0xff,0x00,0x00,0x80, 0xff,0x00,0x00,0x80}; /* RGBA, A 0x80 */
    tex_la_f = make_texture(GL_LUMINANCE_ALPHA, f);
    tex_rgba_half_g = make_texture(GL_RGBA, g);
}

static void reset_units(void)
{
    static const float zero[4] = {0,0,0,0};
    for (int k = 3; k >= 0; --k) {
        glActiveTexture(GL_TEXTURE0 + k);
        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, zero);
        glMatrixMode(GL_TEXTURE);
        glLoadIdentity();
        glClientActiveTexture(GL_TEXTURE0 + k);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    }
    glMatrixMode(GL_MODELVIEW);
    glDisableClientState(GL_COLOR_ARRAY);
    glDisable(GL_BLEND);
    glColor4f(1, 1, 1, 1);
    while (glGetError() != GL_NO_ERROR) {}
}

static void use_unit(int k, GLuint texture, GLenum mode, const float (*coords)[2])
{
    glActiveTexture(GL_TEXTURE0 + k);
    glBindTexture(GL_TEXTURE_2D, texture);
    glEnable(GL_TEXTURE_2D);
    if (mode) glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, (GLint)mode);
    glClientActiveTexture(GL_TEXTURE0 + k);
    glTexCoordPointer(2, GL_FLOAT, 0, coords);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
}

static void env_color(int k, float r, float g, float b, float a)
{
    const float v[4] = {r, g, b, a};
    glActiveTexture(GL_TEXTURE0 + k);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, v);
}

static int read_rgb(int x, unsigned char out[4])
{
    memset(out, 0xa5, 4);
    glReadPixels(x, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
    return glGetError() == GL_NO_ERROR;
}

static void draw_at(int x)
{
    glViewport(x, 0, 4, 4);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* One row: clear, draw, read back.  RPCS3's asynchronous shader compiler may
   skip a draw of a program it has not compiled yet, so a row retries the
   identical draw a bounded number of times; the final pixel must match. */
static void row(const char *name, unsigned rgb)
{
    unsigned char got[4];
    unsigned tries;
    int x = row_x; row_x += 4; if (row_x > 240) row_x = 0;
    for (tries = 1; tries <= 10; ++tries) {
        glClear(GL_COLOR_BUFFER_BIT);
        draw_at(x);
        if (read_rgb(x + 1, got) && got[0] == (unsigned char)(rgb >> 16) &&
            got[1] == (unsigned char)(rgb >> 8) && got[2] == (unsigned char)rgb && got[3] == 255)
            break;
        usleep(200000);
    }
    printf("PSGL_FFP_TEXENV_DRAW %s attempts=%u\n", name, tries > 10 ? 10 : tries);
    if (tries > 10)
        printf("PIXEL_DETAIL %s got=%02x%02x%02x%02x expected=%06x\n", name, got[0], got[1], got[2], got[3], rgb);
    check(tries <= 10, name);
}

static void expect_pixel(int x, unsigned rgb, const char *name)
{
    unsigned char got[4];
    int ok = read_rgb(x, got) && got[0] == (unsigned char)(rgb >> 16) &&
             got[1] == (unsigned char)(rgb >> 8) && got[2] == (unsigned char)rgb && got[3] == 255;
    if (!ok) printf("PIXEL_DETAIL %s got=%02x%02x%02x%02x expected=%06x\n", name, got[0], got[1], got[2], got[3], rgb);
    check(ok, name);
}

__attribute__((unused)) static void error_rows(void)
{
    GLint mode = 0;
    GLfloat color[4] = {0,0,0,0};
    static const float env[4] = {C(0x80), 0.0f, C(0x40), 1.0f};
    reset_units();
    glActiveTexture(GL_TEXTURE1);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_ADD);
    glActiveTexture(GL_TEXTURE2);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_DECAL);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
    check(glGetError() == GL_NO_ERROR, "valid texenv calls raise no error");
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
    check(glGetError() == GL_INVALID_ENUM, "COMBINE is refused with INVALID_ENUM");
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, 0x1234);
    check(glGetError() == GL_INVALID_ENUM, "unknown mode is refused with INVALID_ENUM");
    glTexEnvi(GL_TEXTURE_2D, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    check(glGetError() == GL_INVALID_ENUM, "unknown target is refused with INVALID_ENUM");
    glTexEnvi(GL_TEXTURE_ENV, 0x1234, GL_REPLACE);
    check(glGetError() == GL_INVALID_ENUM, "unknown pname is refused with INVALID_ENUM");
    check(glGetTexEnviv != 0 && glGetTexEnvfv != 0, "glGetTexEnviv/fv are provided");
    if (glGetTexEnviv && glGetTexEnvfv) {
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode);
        check(mode == GL_DECAL, "refused calls keep the unit 2 mode");
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
        check(!memcmp(color, env, sizeof(env)), "unit 2 env colour round trip");
        glActiveTexture(GL_TEXTURE1);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode);
        check(mode == GL_ADD, "unit 1 mode is separate state");
        glActiveTexture(GL_TEXTURE3);
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode);
        check(mode == GL_MODULATE, "unit 3 mode stays MODULATE");
    }
    check(glGetError() == GL_NO_ERROR, "queries raise no error");
}

/* Units 4-7 are texture image units without texture coordinate sets
   (GL 2.1 section 2.11.2): image-unit commands (TexEnv, Enable, BindTexture)
   store their state with no error, coordinate-processing commands (texture
   matrices) are INVALID_OPERATION, and fixed-function draws read units 0-3
   only, so enabling unit 4 changes neither the result nor the program. */
__attribute__((unused)) static void high_unit_rows(void)
{
    static const float env[4] = {C(0x40), C(0x80), C(0xff), 1.0f};
    GLint max_units = 0, max_coords = 0, mode = 0;
    GLfloat color[4] = {0, 0, 0, 0};
    reset_units();
    glGetIntegerv(GL_MAX_TEXTURE_UNITS, &max_units);
    check(max_units == 4, "GL_MAX_TEXTURE_UNITS stays 4");
    glGetIntegerv(GL_MAX_TEXTURE_COORDS_ARB, &max_coords);
    check(max_coords == 4 && glGetError() == GL_NO_ERROR, "GL_MAX_TEXTURE_COORDS reports 4");
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    row("units 4-7: unit-0 reference draw", 0x800040);
    unsigned installs = &psgl_ffp_program_installs != 0 ? psgl_ffp_program_installs : 0u;
    glActiveTexture(GL_TEXTURE4);
    check(glGetError() == GL_NO_ERROR, "unit 4: ActiveTexture selects an image unit");
    glBindTexture(GL_TEXTURE_2D, tex_rgb_b);
    glEnable(GL_TEXTURE_2D);
    check(glGetError() == GL_NO_ERROR, "unit 4: Enable(TEXTURE_2D) stores state with no error");
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, env);
    check(glGetError() == GL_NO_ERROR, "unit 4: TexEnv stores state with no error");
    if (glGetTexEnviv && glGetTexEnvfv) {
        glGetTexEnviv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, &mode);
        check(mode == GL_REPLACE, "unit 4: TexEnv mode round trip");
        glGetTexEnvfv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_COLOR, color);
        check(!memcmp(color, env, sizeof(env)), "unit 4: env colour round trip");
    } else {
        check(0, "unit 4: TexEnv mode round trip");
        check(0, "unit 4: env colour round trip");
    }
    glActiveTexture(GL_TEXTURE7);
    glEnable(GL_TEXTURE_2D);
    check(glGetError() == GL_NO_ERROR, "unit 7: Enable(TEXTURE_2D) stores state with no error");
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_TEXTURE);
    glLoadIdentity();
    check(glGetError() == GL_INVALID_OPERATION, "unit 7: texture matrix op is INVALID_OPERATION");
    glMatrixMode(GL_MODELVIEW);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "units 4-7: enabled unit 4 does not change a unit-0 draw");
    check(&psgl_ffp_program_installs != 0 && psgl_ffp_program_installs == installs,
          "units 4-7: enabling unit 4 does not reinstall the fixed-function programs");

    /* MultiTexCoord names its own coordinate set: with ACTIVE_TEXTURE 7 it
       still sets unit 0's current texcoord, used here without an array. */
    while (glGetError() != GL_NO_ERROR) {}
    glMultiTexCoord4f(GL_TEXTURE0, 0.75f, 0.5f, 0, 1);
    check(glGetError() == GL_NO_ERROR, "MultiTexCoord(TEXTURE0) with ACTIVE_TEXTURE 7: no error");
    glColor4f(0, 0, 0, 1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_rgb_b);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glClientActiveTexture(GL_TEXTURE0);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    row("MultiTexCoord(TEXTURE0) from unit 7 reaches unit 0", 0xff00ff);
    glMultiTexCoord4f(GL_TEXTURE0, 0, 0, 0, 1);

    glActiveTexture(GL_TEXTURE4);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glActiveTexture(GL_TEXTURE0);
}

/* A lit fixed-function draw (normal (0,0,1), light 0 at (0,0,1,0)) is not
   suppressed by an enabled unit 4.  An unlit green draw in between swaps
   the bound programs, so a lit draw left without a program shows green. */
__attribute__((unused)) static void lit_unit4_rows(void)
{
    static const float normals[6][3] = {{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,0,1}};
    static const float position[4] = {0, 0, 1, 0};
    unsigned char lit[4] = {0, 0, 0, 0}, got[4];
    unsigned tries;
    reset_units();
    glNormalPointer(GL_FLOAT, 0, normals);
    glEnableClientState(GL_NORMAL_ARRAY);
    glLightfv(GL_LIGHT0, GL_POSITION, position);
    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHTING);
    for (tries = 1; tries <= 10; ++tries) {
        glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
        if (read_rgb(1, lit) && !(lit[0] == 0 && lit[1] == 0 && lit[2] == 0xff)) break;
        usleep(200000);
    }
    printf("PSGL_FFP_TEXENV_DRAW lit reference attempts=%u value=%02x%02x%02x\n",
           tries > 10 ? 10 : tries, lit[0], lit[1], lit[2]);
    check(tries <= 10, "lit: reference draw");
    glDisable(GL_LIGHTING);
    glColor4f(0, 1, 0, 1);
    row("lit: unlit draw in between", 0x00ff00);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, tex_rgba_a);
    glEnable(GL_TEXTURE_2D);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_LIGHTING);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    int ok = read_rgb(1, got) && !memcmp(got, lit, 3) && got[3] == 255;
    if (!ok) printf("PIXEL_DETAIL lit with unit 4 got=%02x%02x%02x expected=%02x%02x%02x\n",
                    got[0], got[1], got[2], lit[0], lit[1], lit[2]);
    check(ok, "lit: enabled unit 4 does not change a lit draw");
    glDisable(GL_LIGHTING);
    glDisable(GL_LIGHT0);
    glDisableClientState(GL_NORMAL_ARRAY);
    glActiveTexture(GL_TEXTURE4);
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

/* Env colour changes between back-to-back queued draws: each draw keeps the
   colour set before it (BLEND with Ct = (0,1,0), Cp = 0 gives (0, Cc.g, 0)). */
__attribute__((unused)) static void queued_env_rows(void)
{
    static const float greens[4] = {1.0f, 0.0f, C(0x80), C(0x40)};
    reset_units();
    glColor4f(0, 0, 0, 1);
    use_unit(0, tex_rgb_b, GL_BLEND, k_lo);
    env_color(0, 0, 1, 0, 1);
    row("BLEND warm-up before queued env colours", 0x00ff00);
    glClear(GL_COLOR_BUFFER_BIT);
    env_color(0, 0, greens[0], 0, 1); draw_at(0);
    env_color(0, 0, greens[2], 0, 1); draw_at(4);
    expect_pixel(1, 0x00ff00, "queued env colour pair: first draw");
    expect_pixel(5, 0x008000, "queued env colour pair: second draw");
    glClear(GL_COLOR_BUFFER_BIT);
    for (int i = 0; i < 4; ++i) { env_color(0, 0, greens[i], 0, 1); draw_at(i * 4); }
    expect_pixel(1, 0x00ff00, "queued env colour four: draw 0");
    expect_pixel(5, 0x000000, "queued env colour four: draw 1");
    expect_pixel(9, 0x008000, "queued env colour four: draw 2");
    expect_pixel(13, 0x004000, "queued env colour four: draw 3");
}

/* The enabled-unit set changes through binding alone: unit 1 stays enabled
   with ADD while its binding switches between a stored texture and 0 (an
   incomplete unit behaves as disabled), gains storage, or loses its texture
   to deletion.  Nothing else is called between a switch and its draw, so
   only the binding can select the program.  u0 MODULATE RGBA texel 0 with
   colour (1,0,1) gives (80,00,40); adding u1 RGB texel 1 (ff,00,ff) gives
   (ff,00,ff). */
__attribute__((unused)) static void bind_transition_rows(void)
{
    static const unsigned char b[] = {0x00,0xff,0x00, 0xff,0x00,0xff};
    GLuint late = 0, doomed = 0;
    reset_units();
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    use_unit(1, tex_rgb_b, GL_ADD, k_hi);
    row("binding: warm units {0,1}", 0xff00ff);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    row("binding: warm units {0}", 0x800040);
    glBindTexture(GL_TEXTURE_2D, tex_rgb_b);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0xff00ff, "binding: 0 -> texture enables unit 1");
    glBindTexture(GL_TEXTURE_2D, 0);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "binding: texture -> 0 disables unit 1");
    glBindTexture(GL_TEXTURE_2D, tex_rgb_b);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0xff00ff, "binding: 0 -> texture again");
    glBindTexture(GL_TEXTURE_2D, 0);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "binding: texture -> 0 again");

    /* A texture without storage is incomplete until glTexImage2D. */
    glGenTextures(1, &late);
    glBindTexture(GL_TEXTURE_2D, late);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "storage: incomplete unit 1 is disabled");
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, b);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0xff00ff, "storage: glTexImage2D completes unit 1");

    /* Deleting the bound texture leaves unit 1 without one. */
    glGenTextures(1, &doomed);
    glBindTexture(GL_TEXTURE_2D, doomed);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, b);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0xff00ff, "deletion: stored texture on unit 1");
    glDeleteTextures(1, &doomed);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "deletion: deleting it disables unit 1");
    glDeleteTextures(1, &late);

    /* {0} <-> {}: unit 0 alone switches between its texture and none, so
       the untextured program must replace the textured one and back. */
    reset_units();
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    row("binding: warm units {0}", 0x800040);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    row("binding: warm units {}", 0xff00ff);
    glBindTexture(GL_TEXTURE_2D, tex_rgba_a);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "binding: {} -> {0}");
    glBindTexture(GL_TEXTURE_2D, 0);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0xff00ff, "binding: {0} -> {}");
    glBindTexture(GL_TEXTURE_2D, tex_rgba_a);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "binding: {} -> {0} again");
}

__attribute__((unused)) static void render_rows(void)
{
    /* Initial mode is MODULATE (GL 2.1 table 6.19): no TexEnv call yet. */
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_rgba_a, 0, k_lo);
    row("default MODULATE unit 0 (RGBA: Cp*Ct)", 0x800040);

    reset_units();
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    row("MODULATE unit 0 RGBA", 0x800040);

    reset_units();
    glColor4f(0, 0, 0, 1);
    use_unit(0, tex_rgba_a, GL_REPLACE, k_lo);
    row("REPLACE unit 0 RGBA", 0x80ff40);

    reset_units();
    glColor4f(0, C(0x80), 0, 1);
    use_unit(1, tex_rgb_b, GL_ADD, k_hi);
    row("ADD unit 1 alone (RGB: Cp+Ct)", 0xff80ff);

    reset_units();
    glColor4f(1, 0, 0, 1);
    use_unit(1, tex_rgb_b, GL_ADD, k_hi);
    row("ADD unit 1 clamps", 0xff00ff);

    reset_units();
    glColor4f(1, 1, 0, 1);
    use_unit(2, tex_rgba_e, GL_DECAL, k_lo);
    row("DECAL unit 2 alone, At=1 (RGBA: Ct)", 0x0080ff);

    reset_units();
    glColor4f(1, 1, 0, 1);
    use_unit(2, tex_rgba_e, GL_DECAL, k_hi);
    row("DECAL unit 2 alone, At=0 (RGBA: Cp)", 0xffff00);

    reset_units();
    glColor4f(1, 0, C(0x80), 1);
    use_unit(3, tex_rgb_b, GL_BLEND, k_lo);
    env_color(3, C(0x80), C(0x40), 1, 1);
    row("BLEND unit 3 alone (RGB: Cp(1-Ct)+CcCt)", 0xff4080);

    /* Chain: u0 REPLACE RGB (0,ff,0); u1 MODULATE RGBA texel0 -> (0,ff,0);
       u2 BLEND RGB texel1 Ct=(1,0,1), Cc=(80,0,40) -> (80,ff,40);
       u3 ADD LUMINANCE L=80 -> (ff,ff,c0). */
    reset_units();
    glColor4f(1, 1, 1, 1);
    use_unit(0, tex_rgb_b, GL_REPLACE, k_lo);
    use_unit(1, tex_rgba_a, GL_MODULATE, k_lo);
    use_unit(2, tex_rgb_b, GL_BLEND, k_hi);
    env_color(2, C(0x80), 0, C(0x40), 1);
    use_unit(3, tex_lum_d, GL_ADD, k_lo);
    row("chain units 0-3", 0xffffc0);

    /* Gap: units 0 and 2 enabled; 1 and 3 hold textures but are disabled. */
    reset_units();
    glColor4f(1, 0, 1, 1);
    use_unit(1, tex_rgb_b, GL_REPLACE, k_hi);
    glActiveTexture(GL_TEXTURE1); glDisable(GL_TEXTURE_2D);
    use_unit(3, tex_rgba_e, GL_REPLACE, k_hi);
    glActiveTexture(GL_TEXTURE3); glDisable(GL_TEXTURE_2D);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    use_unit(2, tex_rgb_b, GL_ADD, k_lo);
    row("enabled units {0,2}", 0x80ff40);

    /* ALPHA base format: colour passes through, alpha comes from the
       texture; blending SRC_ALPHA over the blue clear exposes Av. */
    reset_units();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 0, 1);
    use_unit(0, tex_alpha_c, GL_REPLACE, k_lo);
    row("ALPHA REPLACE At=1 (Cv=Cp)", 0xff0000);
    use_unit(0, tex_alpha_c, GL_REPLACE, k_hi);
    row("ALPHA REPLACE At=0 (Av=At)", 0x0000ff);
    use_unit(0, tex_alpha_c, GL_MODULATE, k_lo);
    row("ALPHA MODULATE At=1 (Cv=Cp)", 0xff0000);
    glDisable(GL_BLEND);

    reset_units();
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_lum_d, GL_MODULATE, k_lo);
    row("LUMINANCE MODULATE (Cv=Cp*Lt)", 0x800080);

    reset_units();
    glColor4f(0, 0, 0, 1);
    use_unit(0, tex_lum_d, GL_REPLACE, k_hi);
    row("LUMINANCE REPLACE (Cv=Lt)", 0xffffff);

    /* Texture matrix on unit 2 moves s from 0.25 to 0.75. */
    reset_units();
    glColor4f(0, 0, 0, 1);
    glActiveTexture(GL_TEXTURE2);
    glMatrixMode(GL_TEXTURE);
    glTranslatef(0.5f, 0, 0);
    glMatrixMode(GL_MODELVIEW);
    use_unit(2, tex_rgb_b, GL_REPLACE, k_lo);
    row("texture matrix unit 2 selects texel 1", 0xff00ff);
    glActiveTexture(GL_TEXTURE2); glDisable(GL_TEXTURE_2D);
    use_unit(0, tex_rgb_b, GL_REPLACE, k_lo);
    row("unit 0 matrix unaffected by unit 2 matrix", 0x00ff00);

    /* Primary colour from a colour array instead of the current colour. */
    reset_units();
    glColor4f(0, 1, 0, 1);
    glColorPointer(4, GL_UNSIGNED_BYTE, 0, k_magenta);
    glEnableClientState(GL_COLOR_ARRAY);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    row("MODULATE with a colour array", 0x800040);
    glDisableClientState(GL_COLOR_ARRAY);

    /* Per-unit current texcoord (MultiTexCoord) when unit 1 has no array. */
    reset_units();
    glColor4f(0, 0, 0, 1);
    use_unit(1, tex_rgb_b, GL_REPLACE, k_lo);
    glClientActiveTexture(GL_TEXTURE1);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glMultiTexCoord4f(GL_TEXTURE1, 0.75f, 0.5f, 0, 1);
    row("unit 1 current texcoord from MultiTexCoord", 0xff00ff);

    /* Fractional DECAL alpha: Cv = Cp(1-At) + Ct*At with At = 0x80/0xff,
       Cp = (0,1,0), Ct = (1,0,0): (80,7f,00). */
    reset_units();
    glColor4f(0, 1, 0, 1);
    use_unit(0, tex_rgba_half_g, GL_DECAL, k_lo);
    row("DECAL RGBA with fractional At", 0x807f00);

    /* LUMINANCE_ALPHA keeps the texture alpha in REPLACE and modulates it
       in MODULATE (table 3.21); SRC_ALPHA blending over the blue clear
       exposes Av. */
    reset_units();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_la_f, GL_MODULATE, k_hi);
    row("LUMINANCE_ALPHA MODULATE L=ff A=ff (Cp*Lt, Ap*At)", 0xff00ff);
    use_unit(0, tex_la_f, GL_MODULATE, k_lo);
    row("LUMINANCE_ALPHA MODULATE A=00 (Av=Ap*At)", 0x0000ff);
    glColor4f(0, 0, 0, 0);
    use_unit(0, tex_la_f, GL_REPLACE, k_hi);
    row("LUMINANCE_ALPHA REPLACE keeps At with Ap=0", 0xffffff);
    glDisable(GL_BLEND);
}

__attribute__((unused)) static int write_blob(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, size, f) == size;
    return fclose(f) == 0 && ok;
}

int main(void)
{
    PSGLinitOptions options = {0};
    psglInit(&options);
    PSGLdevice *device = psglCreateDeviceAuto(0, 0, 0);
    PSGLcontext *context = psglCreateContext();
    if (!device || !context) { puts("PSGL_FFP_TEXENV_FAIL setup"); return 1; }
    psglMakeCurrent(context, device);
#ifndef PSGL_FFP_TEXENV_NOLIB
    if (!write_blob(LIB_PATH, shaders_bin, shaders_bin_size)) { puts("PSGL_FFP_TEXENV_FAIL library write"); return 1; }
    psglLoadShaderLibrary(LIB_PATH);
#endif
    make_textures();
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_DITHER);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_ALPHA_TEST);
    glClearColor(0, 0, 1, 0);
    glVertexPointer(3, GL_FLOAT, 0, k_quad);
    glEnableClientState(GL_VERTEX_ARRAY);

#ifdef PSGL_FFP_TEXENV_NOLIB
    /* Without a shader library: unit 0 uses the built-in program; a state
       needing units 1-3 is refused visibly (no draw, INVALID_OPERATION). */
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    row("no library: unit 0 MODULATE uses the built-in program", 0x800040);
    reset_units();
    glColor4f(1, 1, 1, 1);
    use_unit(1, tex_rgb_b, GL_REPLACE, k_hi);
    glClear(GL_COLOR_BUFFER_BIT);
    while (glGetError() != GL_NO_ERROR) {}
    glViewport(0, 0, 4, 4);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    check(glGetError() == GL_INVALID_OPERATION, "no library: unit 1 draw is INVALID_OPERATION");
    expect_pixel(1, 0x0000ff, "no library: unit 1 draw leaves the clear colour");
    /* A refused draw must not disturb the installed programs: unit 1 loses
       its texture by binding alone, and the unit-0 draw must be exact. */
    reset_units();
    glColor4f(1, 0, 1, 1);
    use_unit(0, tex_rgba_a, GL_MODULATE, k_lo);
    row("no library: warm unit 0", 0x800040);
    use_unit(1, tex_rgb_b, GL_REPLACE, k_hi);
    glClear(GL_COLOR_BUFFER_BIT);
    while (glGetError() != GL_NO_ERROR) {}
    glViewport(0, 0, 4, 4);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    check(glGetError() == GL_INVALID_OPERATION, "no library: refused {0,1} draw");
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glClear(GL_COLOR_BUFFER_BIT); draw_at(0);
    expect_pixel(1, 0x800040, "no library: unit-0 draw after the refused draw");
#else
    render_rows();
    bind_transition_rows();
    queued_env_rows();
    error_rows();
    high_unit_rows();
    lit_unit4_rows();
#endif
    printf("PSGL_FFP_TEXENV_%s checks=%u failures=%u\n", failures ? "FAIL" : "OK", checks, failures);
    psglExit();
    return failures ? 1 : 0;
}
