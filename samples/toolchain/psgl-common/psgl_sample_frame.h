/* psgl_sample_frame.h - per-frame error check for the PSGL samples
 * (included as "../../psgl-common/psgl_sample_frame.h").
 *
 * Call psgl_sample_frame_ok() after each frame's draws: it reads every
 * pending GL error, prints the first one with its frame number, and returns
 * 0 when the frame had an error, so the sample can end with "fail" and a
 * nonzero status.  A build with -DPSGL_SAMPLE_FAIL_FRAME=<n> raises
 * GL_INVALID_VALUE on frame n, to show that a later-frame failure reaches
 * the final line; normal builds leave it undefined.
 */
#ifndef PSGL_SAMPLE_FRAME_H
#define PSGL_SAMPLE_FRAME_H

#include <stdio.h>
#include <GLES/gl.h>

static inline int psgl_sample_frame_ok(const char *name, int frame)
{
#ifdef PSGL_SAMPLE_FAIL_FRAME
    if (frame == PSGL_SAMPLE_FAIL_FRAME)
        glDrawArrays(GL_TRIANGLES, 0, -1);   /* GL_INVALID_VALUE */
#endif
    GLenum first = glGetError();
    if (first == GL_NO_ERROR) return 1;
    while (glGetError() != GL_NO_ERROR) {}
    printf("%s: GL error 0x%04x on frame %d\n", name, (unsigned)first, frame);
    return 0;
}

#endif
