#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

#include "../../psgl-common/psgl_sample_quit.h"

#include "fshader_fpo.h"
#include "vshader_vpo.h"

SYS_PROCESS_PARAM(1001, 0x100000);

typedef struct Vertex {
    float position[3];
} Vertex;

static const Vertex k_vertices[3] = {
    {{-0.65f, -0.55f, 0.0f}},
    {{ 0.65f, -0.55f, 0.0f}},
    {{ 0.00f,  0.65f, 0.0f}}
};

static int check_program(GLuint program)
{
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) return 1;
    printf("hello-psgl-glsl-object: binary link failed\n");
    return 0;
}

static GLuint load_shader(GLenum type, const unsigned char *binary,
                           unsigned int binary_size)
{
    GLuint shader = glCreateShader(type);
    GLenum error;
    if (!shader) return 0;
    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_CGB_PS3DK,
                   binary, (GLsizei)binary_size);
    error = glGetError();
    if (error != GL_NO_ERROR) {
        printf("hello-psgl-glsl-object: binary load failed: 0x%x\n", error);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

int main(void)
{
    PSGLinitOptions options = {0};
    PSGLdevice *device;
    PSGLcontext *gl_context;
    GLuint vertex_shader;
    GLuint fragment_shader;
    GLuint program;
    GLuint vbo = 0u;
    GLint mvp_location;
    GLint color_location;
    const float identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    };

    psglInit(&options);
    device = psglCreateDeviceAuto(0, 0, 0);
    gl_context = psglGetCurrentContext();
    if (!gl_context) gl_context = psglCreateContext();
    if (!device || !gl_context) {
        printf("hello-psgl-glsl-object: PSGL init failed\n");
        psglExit();
        return 1;
    }
    psglMakeCurrent(gl_context, device);

    vertex_shader = load_shader(GL_VERTEX_SHADER,
                                   vshader_vpo, vshader_vpo_size);
    fragment_shader = load_shader(GL_FRAGMENT_SHADER,
                                     fshader_fpo, fshader_fpo_size);
    if (!vertex_shader || !fragment_shader) {
        psglExit();
        return 1;
    }

    program = glCreateProgram();
    glBindAttribLocation(program, 0, "in_position");
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);
    if (!check_program(program)) {
        psglExit();
        return 1;
    }
    glUseProgram(program);

    mvp_location = glGetUniformLocation(program, "modelViewProj");
    color_location = glGetUniformLocation(program, "solidColor");
    if (mvp_location < 0 || color_location < 0) {
        printf("hello-psgl-glsl-object: uniform lookup failed\n");
        psglExit();
        return 1;
    }
    glUniformMatrix4fv(mvp_location, 1, GL_FALSE, identity);
    glUniform4f(color_location, 0.0f, 1.0f, 0.0f, 1.0f);

    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(k_vertices), k_vertices,
                 GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)0);
    glEnableVertexAttribArray(0);

    psgl_sample_viewport(device);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);

    printf("hello-psgl-glsl-object: ready\n");
    int exit_code = psgl_sample_quit_begin("hello-psgl-glsl-object");
    for (int i = 0; i < 900 && exit_code == 0; i++) {
        int quit = psgl_sample_quit_poll("hello-psgl-glsl-object");
        if (quit < 0) { exit_code = 1; break; }
        if (quit > 0) break;
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        psglSwap();
        sys_timer_usleep(16000);
    }
    psgl_sample_quit_end();
    printf("hello-psgl-glsl-object: %s\n", psgl_sample_quit_outcome(exit_code));

    psglExit();
    return exit_code;
}
