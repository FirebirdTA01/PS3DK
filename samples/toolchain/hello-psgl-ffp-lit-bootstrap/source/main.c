#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

SYS_PROCESS_PARAM(1001, 0x100000);

typedef struct Vertex {
    float position[3];
    float normal[4];
} Vertex;

static const Vertex k_vertices[6] = {
    {{-0.80f, -0.80f, 0.0f}, {0.15f, 0.0f, 0.0f, 1.0f}},
    {{ 0.80f, -0.80f, 0.0f}, {1.00f, 0.0f, 0.0f, 1.0f}},
    {{-0.80f,  0.80f, 0.0f}, {0.15f, 0.0f, 0.0f, 1.0f}},
    {{-0.80f,  0.80f, 0.0f}, {0.15f, 0.0f, 0.0f, 1.0f}},
    {{ 0.80f, -0.80f, 0.0f}, {1.00f, 0.0f, 0.0f, 1.0f}},
    {{ 0.80f,  0.80f, 0.0f}, {1.00f, 0.0f, 0.0f, 1.0f}}
};

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    GLuint vbo = 0u;
    const GLfloat light_position[4] = {1.0f, 0.0f, 0.0f, 0.0f};

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf("hello-psgl-ffp-lit-bootstrap: PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_vertices), k_vertices, GL_STATIC_DRAW);
    glVertexPointer(3, GL_FLOAT, sizeof(Vertex), (const void *)0);
    glNormalPointer(GL_FLOAT, sizeof(Vertex),
                    (const void *)(uintptr_t)sizeof(((Vertex *)0)->position));
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_NORMAL_ARRAY);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glViewport(0, 0, 1280, 720);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    glLightfv(GL_LIGHT0, GL_POSITION, light_position);
    glEnable(GL_LIGHTING);
    glEnable(GL_LIGHT0);

    printf("hello-psgl-ffp-lit-bootstrap: ready\n");
    for (int i = 0; i < 900; i++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        psglSwap();
        sys_timer_usleep(16000);
    }

    printf("hello-psgl-ffp-lit-bootstrap: done\n");
    psglExit();
    return 0;
}
