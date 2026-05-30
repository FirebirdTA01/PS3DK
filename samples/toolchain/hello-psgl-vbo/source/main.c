#include <PSGL/psgl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <ppu-lv2.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/process.h>

SYS_PROCESS_PARAM(1001, 0x100000);

typedef struct Vertex {
    GLfloat position[3];
    GLubyte color[4];
} Vertex;

static int vertex_equal(const Vertex *a, const Vertex *b)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < sizeof(*a); i++) {
        if (pa[i] != pb[i]) return 0;
    }
    return 1;
}

int main(void)
{
    static const Vertex vertices[3] = {
        {{-0.5f, -0.5f, 0.0f}, {255u, 0u, 0u, 255u}},
        {{ 0.5f, -0.5f, 0.0f}, {0u, 255u, 0u, 255u}},
        {{ 0.0f,  0.5f, 0.0f}, {0u, 0u, 255u, 255u}}
    };
    static const Vertex patch_vertex = {
        {0.0f, 0.5f, 0.0f}, {32u, 64u, 255u, 255u}
    };

    PSGLinitOptions options = {0};
    psglInit(&options);

    GLuint buffer = 0;
    GLint size = 0;
    GLint usage = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)sizeof(vertices),
                 vertices, GL_STATIC_DRAW);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_SIZE, &size);
    glGetBufferParameteriv(GL_ARRAY_BUFFER, GL_BUFFER_USAGE, &usage);

    if (size != (GLint)sizeof(vertices) || usage != GL_STATIC_DRAW) {
        printf("hello-psgl-vbo: query failed buffer=%u size=%d usage=0x%x\n",
               buffer, size, usage);
        glDeleteBuffers(1, &buffer);
        psglExit();
        return 1;
    }

    glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(sizeof(Vertex) * 2u),
                    (GLsizeiptr)sizeof(patch_vertex), &patch_vertex);
    Vertex *mapped = (Vertex *)glMapBuffer(GL_ARRAY_BUFFER, GL_READ_WRITE);
    if (!mapped || !vertex_equal(&mapped[2], &patch_vertex) ||
        glUnmapBuffer(GL_ARRAY_BUFFER) != GL_TRUE) {
        printf("hello-psgl-vbo: map failed buffer=%u mapped=%p\n",
               buffer, (void *)mapped);
        glDeleteBuffers(1, &buffer);
        psglExit();
        return 1;
    }

    glVertexPointer(3, GL_FLOAT, (GLsizei)sizeof(Vertex), (const GLvoid *)0);
    glColorPointer(4, GL_UNSIGNED_BYTE, (GLsizei)sizeof(Vertex),
                   (const GLvoid *)(uintptr_t)offsetof(Vertex, color));
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_COLOR_ARRAY);
    glDisableClientState(GL_COLOR_ARRAY);

    glDeleteBuffers(1, &buffer);
    psglExit();
    printf("hello-psgl-vbo: ok buffer=%u size=%d\n", buffer, size);
    return 0;
}
