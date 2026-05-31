#include <GLES/gl.h>
#include <GLES/glext.h>
#include <PSGL/psgl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/process.h>
#include <sys/timer.h>

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

static const char *k_vertex_source =
    "attribute vec3 in_position;\n"
    "uniform mat4 modelViewProj;\n"
    "void main() {\n"
    "    gl_Position = modelViewProj * vec4(in_position, 1.0);\n"
    "}\n";

static const char *k_fragment_source =
    "uniform vec4 solidColor;\n"
    "void main() {\n"
    "    gl_FragColor = solidColor;\n"
    "}\n";

static int check_shader(GLuint shader, const char *name)
{
    GLint ok = 0;
    GLchar log[96];
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok) return 1;
    glGetShaderInfoLog(shader, (GLsizei)sizeof(log), NULL, log);
    printf("hello-psgl-glsl-object: %s compile failed: %s\n", name, log);
    return 0;
}

static int check_program(GLuint program)
{
    GLint ok = 0;
    GLchar log[96];
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok) return 1;
    glGetProgramInfoLog(program, (GLsizei)sizeof(log), NULL, log);
    printf("hello-psgl-glsl-object: link failed: %s\n", log);
    return 0;
}

static GLuint compile_shader(GLenum type, const char *source,
                             const unsigned char *binary,
                             unsigned int binary_size)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, NULL);
    glShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_CGB_PS3DK,
                   binary, (GLsizei)binary_size);
    glCompileShader(shader);
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

    vertex_shader = compile_shader(GL_VERTEX_SHADER, k_vertex_source,
                                   vshader_vpo, vshader_vpo_size);
    fragment_shader = compile_shader(GL_FRAGMENT_SHADER, k_fragment_source,
                                     fshader_fpo, fshader_fpo_size);
    if (!check_shader(vertex_shader, "vertex") ||
        !check_shader(fragment_shader, "fragment")) {
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

    glViewport(0, 0, 1280, 720);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_ALPHA_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.02f, 0.08f, 0.18f, 1.0f);

    printf("hello-psgl-glsl-object: ready\n");
    for (int i = 0; i < 900; i++) {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        psglSwap();
        sys_timer_usleep(16000);
    }
    printf("hello-psgl-glsl-object: done\n");

    psglExit();
    return 0;
}
