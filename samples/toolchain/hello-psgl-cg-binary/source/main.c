#include <Cg/cgGL.h>
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <ppu-lv2.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "../../psgl-common/psgl_sample_frame.h"
#include "../../psgl-common/psgl_sample_quit.h"
#include "../../psgl-common/psgl_sample_tmpfile.h"

#include "tintfp_fpo.h"
#include "tintvp_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

#define NAME "hello-psgl-cg-binary"

/* Shaders are compiled offline (rsx-cg-compiler, raw .vpo/.fpo containers)
   and created at run time from those binaries, the way an application
   ships them.  The left triangle's programs come from the embedded buffers
   through cgCreateProgram; the right triangle's from files through
   cgCreateProgramFromFile.  Programs are never compiled on the console. */

typedef struct Vertex {
    float position[3];
} Vertex;

static const Vertex k_vertices[3] = {
    {{-0.30f, -0.45f, 0.0f}},
    {{ 0.30f, -0.45f, 0.0f}},
    {{ 0.00f,  0.45f, 0.0f}}
};

typedef struct ProgramPair {
    CGprogram vp;
    CGprogram fp;
    CGparameter mvp;
    CGparameter tint;
} ProgramPair;

/* Prints the pending Cg error, if any, and clears it. */
static int cg_failed(const char *what)
{
    CGerror error = CG_NO_ERROR;
    const char *text = cgGetLastErrorString(&error);
    if (error == CG_NO_ERROR) return 0;
    printf(NAME ": %s failed: %s (%d)\n", what, text ? text : "?", (int)error);
    return 1;
}

static int finish_pair(ProgramPair *pair, const char *what)
{
    if (!pair->vp || !pair->fp) {
        if (!cg_failed(what)) printf(NAME ": %s returned NULL\n", what);
        return 0;
    }
    pair->mvp = cgGetNamedParameter(pair->vp, "modelViewProj");
    pair->tint = cgGetNamedParameter(pair->fp, "tint");
    if (!pair->mvp || !pair->tint) {
        printf(NAME ": %s: missing parameter\n", what);
        return 0;
    }
    cgGLLoadProgram(pair->vp);
    cgGLLoadProgram(pair->fp);
    return !cg_failed(what);
}

static void draw_pair(const ProgramPair *pair, float x_offset,
                      const float tint[4])
{
    const float mvp[16] = {
        1.0f, 0.0f, 0.0f, x_offset,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };
    cgGLBindProgram(pair->vp);
    cgGLBindProgram(pair->fp);
    cgGLSetMatrixParameterfr(pair->mvp, mvp);
    cgGLSetParameter4fv(pair->tint, tint);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* Cg source is not compiled at run time: the request must be refused with
   NULL and CG_INVALID_ENUMERANT_ERROR. */
static int source_refused(CGcontext cg_context)
{
    CGprogram refused = cgCreateProgram(cg_context, CG_SOURCE, "void main() {}",
                                        CG_PROFILE_SCE_FP_RSX, "main", NULL);
    CGerror error = CG_NO_ERROR;
    const char *text = cgGetLastErrorString(&error);
    if (refused || error != CG_INVALID_ENUMERANT_ERROR) {
        printf(NAME ": CG_SOURCE: program=%s error=%d, want NULL and %d\n",
               refused ? "non-NULL" : "NULL", (int)error,
               (int)CG_INVALID_ENUMERANT_ERROR);
        return 0;
    }
    printf(NAME ": CG_SOURCE refused as expected: %s\n", text ? text : "?");
    return 1;
}

int main(void)
{
    static const float k_green[4] = {0.15f, 0.85f, 0.30f, 1.0f};
    static const float k_orange[4] = {1.00f, 0.55f, 0.10f, 1.0f};
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    CGcontext cg_context;
    ProgramPair from_memory = {0};
    ProgramPair from_file = {0};
    GLuint vbo = 0u;
    char vp_path[64] = "";
    char fp_path[64] = "";
    int exit_code = 1;

    /* cgCreateProgramFromFile takes a path: write the containers to new
       private files, removed again at exit. */
    if (!psgl_sample_tmpfile_create(&psgl_sample_lv2_fs, NAME, "vpo", tintvp_vpo,
                                    tintvp_vpo_size, vp_path, sizeof vp_path) ||
        !psgl_sample_tmpfile_create(&psgl_sample_lv2_fs, NAME, "fpo", tintfp_fpo,
                                    tintfp_fpo_size, fp_path, sizeof fp_path)) {
        psgl_sample_tmpfile_remove(&psgl_sample_lv2_fs, vp_path);
        printf(NAME ": fail\n");
        return 1;
    }

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    cg_context = NULL;
    if (!device || !gl_context) {
        printf(NAME ": PSGL init failed\n");
        goto out;
    }
    psglMakeCurrent(gl_context, device);

    cg_context = cgCreateContext();
    if (!cg_context) {
        printf(NAME ": cgCreateContext failed\n");
        goto out;
    }

    /* From memory: the container is copied, so the buffer may be freed
       once the program exists.  CG_PROFILE_UNKNOWN would also work: the
       profile is then taken from the container. */
    from_memory.vp = cgCreateProgram(cg_context, CG_BINARY,
                                     (const char *)tintvp_vpo,
                                     CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    from_memory.fp = cgCreateProgram(cg_context, CG_BINARY,
                                     (const char *)tintfp_fpo,
                                     CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    /* From files: CG_OBJECT and CG_BINARY are both accepted. */
    from_file.vp = cgCreateProgramFromFile(cg_context, CG_OBJECT, vp_path,
                                           CG_PROFILE_SCE_VP_RSX, NULL, NULL);
    from_file.fp = cgCreateProgramFromFile(cg_context, CG_OBJECT, fp_path,
                                           CG_PROFILE_SCE_FP_RSX, NULL, NULL);
    if (!finish_pair(&from_memory, "cgCreateProgram") ||
        !finish_pair(&from_file, "cgCreateProgramFromFile") ||
        !source_refused(cg_context))
        goto out;

    cgGLEnableProfile(CG_PROFILE_SCE_VP_RSX);
    cgGLEnableProfile(CG_PROFILE_SCE_FP_RSX);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_vertices), k_vertices,
                 GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void *)0);
    glEnableClientState(GL_VERTEX_ARRAY);

    psgl_sample_viewport(device);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    if (!psgl_sample_frame_ok(NAME, -1) || cg_failed("setup"))
        goto out;

    printf(NAME ": ready\n");
    exit_code = psgl_sample_quit_begin(NAME);
    for (int i = 0; i < 900 && exit_code == 0; i++) {
        int quit = psgl_sample_quit_poll(NAME);
        if (quit < 0) { exit_code = 1; break; }
        if (quit > 0) break;
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        draw_pair(&from_memory, -0.45f, k_green);
        draw_pair(&from_file, 0.45f, k_orange);
        /* Every frame: a failed draw shows up as a GL error (for example
           GL_OUT_OF_MEMORY when a program cannot be installed), a failed
           Cg call as a Cg error. */
        if (!psgl_sample_frame_ok(NAME, i) || cg_failed("draw")) {
            exit_code = 1;
            break;
        }
        psglSwap();
        sys_timer_usleep(16000);
    }
    psgl_sample_quit_end();

out:
    if (cg_context) cgDestroyContext(cg_context);
    psglExit();
    int removed = psgl_sample_tmpfile_remove(&psgl_sample_lv2_fs, vp_path);
    removed &= psgl_sample_tmpfile_remove(&psgl_sample_lv2_fs, fp_path);
    if (!removed) {
        printf(NAME ": removing a shader file failed\n");
        exit_code = 1;
    }
    printf(NAME ": %s\n", psgl_sample_quit_outcome(exit_code));
    return exit_code;
}
