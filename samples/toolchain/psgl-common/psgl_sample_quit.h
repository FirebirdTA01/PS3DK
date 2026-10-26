/* psgl_sample_quit.h - shared Quit handling and viewport sizing for the PSGL
 * rendering samples (included as "../../psgl-common/psgl_sample_quit.h").
 *
 * Render loop contract:
 *
 *     int exit_code = psgl_sample_quit_begin(NAME);
 *     for (int i = 0; i < FRAMES && exit_code == 0; i++) {
 *         int quit = psgl_sample_quit_poll(NAME);
 *         if (quit < 0) { exit_code = 1; break; }
 *         if (quit > 0) break;
 *         ... draw and swap ...
 *     }
 *     psgl_sample_quit_end();
 *     printf("%s: %s\n", NAME, psgl_sample_quit_outcome(exit_code));
 *     ... cleanup ...
 *     return exit_code;
 *
 * A failed registration or poll makes the sample exit nonzero ("fail"); Quit
 * chosen from the XMB ends the loop before the next draw ("quit"); otherwise
 * the loop runs to its frame count ("done").  Only a callback that was
 * registered is unregistered.
 */
#ifndef PSGL_SAMPLE_QUIT_H
#define PSGL_SAMPLE_QUIT_H

#include <stdint.h>
#include <stdio.h>
#include <GLES/gl.h>
#include <PSGL/psgl.h>
#include <cell/sysutil.h>

static volatile int psgl_sample_exit_requested = 0;
static int psgl_sample_callback_registered = 0;

static void psgl_sample_on_sysutil(uint64_t status, uint64_t param, void *userdata)
{
    (void)param;
    (void)userdata;
    if (status == CELL_SYSUTIL_REQUEST_EXITGAME) psgl_sample_exit_requested = 1;
}

/* 0 when the callback is registered; 1 (the sample's exit code) when not. */
static inline int psgl_sample_quit_begin(const char *name)
{
    int rc = cellSysutilRegisterCallback(0, psgl_sample_on_sysutil, NULL);
    if (rc != 0) {
        printf("%s: sysutil callback registration failed 0x%08x\n", name, (unsigned)rc);
        return 1;
    }
    psgl_sample_callback_registered = 1;
    return 0;
}

/* Once per frame, before drawing: 0 draw, 1 Quit was chosen, -1 poll failed. */
static inline int psgl_sample_quit_poll(const char *name)
{
    int rc = cellSysutilCheckCallback();
    if (rc != 0) {
        printf("%s: sysutil callback poll failed 0x%08x\n", name, (unsigned)rc);
        return -1;
    }
    return psgl_sample_exit_requested ? 1 : 0;
}

static inline void psgl_sample_quit_end(void)
{
    if (psgl_sample_callback_registered) {
        cellSysutilUnregisterCallback(0);
        psgl_sample_callback_registered = 0;
    }
}

static inline const char *psgl_sample_quit_outcome(int exit_code)
{
    if (exit_code) return "fail";
    return psgl_sample_exit_requested ? "quit" : "done";
}

/* The render buffer follows the display mode (720p, 1080p, ...). */
static inline void psgl_sample_viewport(const PSGLdevice *device)
{
    GLuint width = 0u;
    GLuint height = 0u;
    psglGetRenderBufferDimensions(device, &width, &height);
    glViewport(0, 0, (GLsizei)width, (GLsizei)height);
}

#endif
