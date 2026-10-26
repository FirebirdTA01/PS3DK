#include "cg_internal.h"
#include "psgl_context.h"

#include <GLES/glext.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>

#define PSGL_GL_MAX_ATTACHED_SHADERS 2u
#define PSGL_GL_MAX_UNIFORMS 32u
#define PSGL_GL_MAX_ATTRIB_BINDINGS 8u

typedef struct PSGLglShader {
    GLuint name;
    GLenum type;
    CGprogram cg_program;
    GLboolean compiled;
    GLboolean deleted;
    struct PSGLglShader *next;
} PSGLglShader;

typedef struct PSGLglUniform {
    char name[64];
    CGparameter vertex_parameter;
    CGparameter fragment_parameter;
} PSGLglUniform;

typedef struct PSGLglAttribBinding {
    char name[64];
    GLuint index;
} PSGLglAttribBinding;

typedef struct PSGLglProgram {
    GLuint name;
    PSGLglShader *attached[PSGL_GL_MAX_ATTACHED_SHADERS];
    uint32_t attached_count;
    CGprogram vertex_program;
    CGprogram fragment_program;
    GLboolean linked;
    GLboolean deleted;
    char log[96];
    PSGLglUniform uniforms[PSGL_GL_MAX_UNIFORMS];
    uint32_t uniform_count;
    PSGLglAttribBinding attrib_bindings[PSGL_GL_MAX_ATTRIB_BINDINGS];
    uint32_t attrib_binding_count;
    PSGLglAttribBinding linked_attribs[PSGL_MAX_GL_ATTRIBS];
    uint32_t linked_attrib_count;
    int8_t hw_to_location[PSGL_MAX_GENERIC_ATTRIBS];
    struct PSGLglProgram *next;
} PSGLglProgram;

static PSGLglShader *g_shaders;
static PSGLglProgram *g_programs;
static GLuint g_next_shader = 1u;
static GLuint g_next_program = 1u;
static CGcontext g_shader_context;
static GLuint g_current_program;

static uint32_t gls_len(const char *s)
{
    uint32_t n = 0u;
    if (!s) return 0u;
    while (s[n] != '\0') n++;
    return n;
}

static void gls_copy(void *dst, const void *src, uint32_t size)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    while (size--) *out++ = *in++;
}

static int gls_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static void gls_copy_name(char *dst, const char *src, uint32_t capacity)
{
    uint32_t i = 0u;
    if (!dst || !capacity) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (; i + 1u < capacity && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void gls_set_log(char *dst, uint32_t capacity, const char *message)
{
    gls_copy_name(dst, message, capacity);
}

static CGcontext gls_context(void)
{
    if (!g_shader_context) g_shader_context = cgCreateContext();
    return g_shader_context;
}

static PSGLglShader *gls_find_shader(GLuint name)
{
    PSGLglShader *s = g_shaders;
    while (s) {
        if (s->name == name) return s;
        s = s->next;
    }
    return NULL;
}

static PSGLglProgram *gls_find_program(GLuint name)
{
    PSGLglProgram *p = g_programs;
    while (p) {
        if (p->name == name) return p;
        p = p->next;
    }
    return NULL;
}

static GLint gls_make_uniform_location(PSGLglProgram *program, uint32_t index)
{
    return (GLint)((program->name << 8) | ((index + 1u) & 0xffu));
}

static PSGLglUniform *gls_find_uniform_by_location(GLint location)
{
    GLuint program_name;
    uint32_t index;
    PSGLglProgram *program;
    if (location <= 0) return NULL;
    program_name = (GLuint)((uint32_t)location >> 8);
    index = ((uint32_t)location & 0xffu);
    if (!index) return NULL;
    program = gls_find_program(program_name);
    if (!program || index > program->uniform_count) return NULL;
    return &program->uniforms[index - 1u];
}

static CGparameter gls_named_parameter(CGprogram program, const char *name)
{
    return program ? cgGetNamedParameter(program, name) : NULL;
}

GLAPI GLuint glCreateShader(GLenum type)
{
    PSGLglShader *shader;
    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) return 0u;
    shader = (PSGLglShader *)calloc(1u, sizeof(PSGLglShader));
    if (!shader) return 0u;
    shader->name = g_next_shader++;
    shader->type = type;
    shader->next = g_shaders;
    g_shaders = shader;
    return shader->name;
}

GLAPI void glDeleteShader(GLuint shader)
{
    PSGLglShader *s = gls_find_shader(shader);
    if (!s) return;
    s->deleted = GL_TRUE;
}

GLAPI GLboolean glIsShader(GLuint shader)
{ return gls_find_shader(shader) ? GL_TRUE : GL_FALSE; }

/* ES 2.0.25 sections 2.10.1 and 6.1.8: no on-target compiler. */
GLAPI void glShaderSource(GLuint shader, GLsizei count,
                          const GLchar **string, const GLint *length)
{
    (void)shader; (void)count; (void)string; (void)length;
    psgl_gl_error(GL_INVALID_OPERATION);
}

GLAPI void glCompileShader(GLuint shader)
{
    (void)shader;
    psgl_gl_error(GL_INVALID_OPERATION);
}

GLAPI void glReleaseShaderCompiler(void)
{
    psgl_gl_error(GL_INVALID_OPERATION);
}

GLAPI void glGetShaderSource(GLuint shader, GLsizei bufSize,
                             GLsizei *length, GLchar *source)
{
    (void)shader; (void)bufSize; (void)length; (void)source;
    psgl_gl_error(GL_INVALID_OPERATION);
}

GLAPI void glGetShaderPrecisionFormat(GLenum shadertype, GLenum precisiontype,
                                      GLint *range, GLint *precision)
{
    (void)shadertype; (void)precisiontype; (void)range; (void)precision;
    psgl_gl_error(GL_INVALID_OPERATION);
}

GLAPI void glShaderBinary(GLsizei count, const GLuint *shaders,
                          GLenum binaryformat, const GLvoid *binary,
                          GLsizei length)
{
    PSGLglShader *shader;
    CellCgbProgram parsed;
    CellCgbProfile stage;
    CGprofile profile;
    CGprogram loaded;
    CGcontext context;
    void *copy;
    if (count < 0 || length < 0) {
        psgl_gl_error(GL_INVALID_VALUE); return;
    }
    if (binaryformat != GL_SHADER_BINARY_FORMAT_CGB_PS3DK) {
        psgl_gl_error(GL_INVALID_ENUM); return;
    }
    if (!count) return;
    if (!shaders || count > 2) {
        psgl_gl_error(GL_INVALID_VALUE); return;
    }
    shader = gls_find_shader(shaders[0]);
    if (!shader) { psgl_gl_error(GL_INVALID_VALUE); return; }
    if (count == 2) {
        PSGLglShader *second = gls_find_shader(shaders[1]);
        if (!second) { psgl_gl_error(GL_INVALID_VALUE); return; }
        /* This format is one container for one stage, never a linked pair. */
        psgl_gl_error(second->type == shader->type ?
                      GL_INVALID_OPERATION : GL_INVALID_VALUE);
        return;
    }
    if (!binary || !length) { psgl_gl_error(GL_INVALID_VALUE); return; }
    /* Client storage need not be aligned. Parse only an aligned, owned copy. */
    copy = malloc((size_t)length);
    if (!copy) { psgl_gl_error(GL_OUT_OF_MEMORY); return; }
    gls_copy(copy, binary, (uint32_t)length);
    if (cellCgbRead(copy, (uint32_t)length, &parsed) != CELL_CGB_OK) {
        free(copy); psgl_gl_error(GL_INVALID_VALUE); return;
    }
    stage = cellCgbGetProfile(&parsed);
    if ((shader->type == GL_VERTEX_SHADER && stage != CELL_CGB_PROFILE_VERTEX) ||
        (shader->type == GL_FRAGMENT_SHADER && stage != CELL_CGB_PROFILE_FRAGMENT)) {
        free(copy); psgl_gl_error(GL_INVALID_OPERATION); return;
    }
    context = gls_context();
    if (!context) { free(copy); psgl_gl_error(GL_OUT_OF_MEMORY); return; }
    profile = shader->type == GL_VERTEX_SHADER ?
        CG_PROFILE_SCE_VP_RSX : CG_PROFILE_SCE_FP_RSX;
    loaded = psgl_cg_create_program_from_memory(context, copy, (uint32_t)length, profile);
    free(copy);
    if (!loaded) {
        psgl_gl_error(cgGetError() == CG_MEMORY_ALLOC_ERROR ?
                      GL_OUT_OF_MEMORY : GL_INVALID_VALUE);
        return;
    }
    /* The Cg context owns program allocations. A previous executable may
     * still be referenced by an already linked GL program after a reload. */
    shader->cg_program = loaded;
    shader->compiled = GL_TRUE;
}

GLAPI void glGetShaderiv(GLuint shader, GLenum pname, GLint *params)
{
    PSGLglShader *s;
    if (pname == GL_COMPILE_STATUS || pname == GL_INFO_LOG_LENGTH ||
        pname == GL_SHADER_SOURCE_LENGTH) {
        psgl_gl_error(GL_INVALID_OPERATION); return;
    }
    s = gls_find_shader(shader);
    if (!s) { psgl_gl_error(GL_INVALID_VALUE); return; }
    if (!params) return;
    switch (pname) {
    case GL_SHADER_TYPE: *params = (GLint)s->type; break;
    case GL_DELETE_STATUS: *params = (GLint)s->deleted; break;
    default: psgl_gl_error(GL_INVALID_ENUM); break;
    }
}

GLAPI void glGetShaderInfoLog(GLuint shader, GLsizei bufSize,
                              GLsizei *length, GLchar *infoLog)
{
    (void)shader; (void)bufSize; (void)length; (void)infoLog;
    psgl_gl_error(GL_INVALID_OPERATION);
}

GLAPI GLuint glCreateProgram(void)
{
    PSGLglProgram *program =
        (PSGLglProgram *)calloc(1u, sizeof(PSGLglProgram));
    if (!program) return 0u;
    program->name = g_next_program++;
    program->next = g_programs;
    g_programs = program;
    return program->name;
}

GLAPI void glDeleteProgram(GLuint program)
{
    PSGLglProgram *p = gls_find_program(program);
    if (!p) return;
    p->deleted = GL_TRUE;
    if (g_current_program == program) glUseProgram(0u);
}

GLAPI GLboolean glIsProgram(GLuint program)
{ return gls_find_program(program) ? GL_TRUE : GL_FALSE; }

GLAPI void glAttachShader(GLuint program, GLuint shader)
{
    PSGLglProgram *p = gls_find_program(program);
    PSGLglShader *s = gls_find_shader(shader);
    if (!p || !s || p->attached_count >= PSGL_GL_MAX_ATTACHED_SHADERS) return;
    for (uint32_t i = 0; i < p->attached_count; i++)
        if (p->attached[i] == s) return;
    p->attached[p->attached_count++] = s;
}

GLAPI void glDetachShader(GLuint program, GLuint shader)
{
    PSGLglProgram *p = gls_find_program(program);
    PSGLglShader *s = gls_find_shader(shader);
    if (!p || !s) return;
    for (uint32_t i = 0; i < p->attached_count; i++) {
        if (p->attached[i] == s) {
            for (uint32_t j = i + 1u; j < p->attached_count; j++)
                p->attached[j - 1u] = p->attached[j];
            p->attached_count--;
            return;
        }
    }
}

GLAPI void glBindAttribLocation(GLuint program, GLuint index,
                                const GLchar *name)
{
    PSGLglProgram *p = gls_find_program(program);
    PSGLglAttribBinding *binding;
    if (!p || index >= PSGL_MAX_GL_ATTRIBS) {
        psgl_gl_error(GL_INVALID_VALUE); return;
    }
    if (!name) return;
    if (name[0] == 'g' && name[1] == 'l' && name[2] == '_') {
        psgl_gl_error(GL_INVALID_OPERATION); return;
    }
    for (uint32_t i = 0; i < p->attrib_binding_count; ++i) {
        if (gls_streq(p->attrib_bindings[i].name, name)) {
            p->attrib_bindings[i].index = index;
            return;
        }
    }
    if (p->attrib_binding_count >= PSGL_GL_MAX_ATTRIB_BINDINGS || gls_len(name) >= 64) {
        psgl_gl_error(GL_OUT_OF_MEMORY); return;
    }
    binding = &p->attrib_bindings[p->attrib_binding_count++];
    binding->index = index;
    gls_copy_name(binding->name, name, sizeof(binding->name));
}

GLAPI void glLinkProgram(GLuint program)
{
    PSGLglProgram *p = gls_find_program(program);
    CGprogram vertex = NULL, fragment = NULL;
    PSGLglAttribBinding linked[PSGL_MAX_GL_ATTRIBS];
    uint32_t count = 0, used = 0, reflected = 0;
    uint8_t hardware[PSGL_MAX_GL_ATTRIBS];
    int8_t route[PSGL_MAX_GENERIC_ATTRIBS];
    CellCgbVertexProgramConfiguration conf;
    if (!p) { psgl_gl_error(GL_INVALID_VALUE); return; }
    /* Construct a fresh executable. A failed relink leaves the last good
       executable installed if this program is currently in use. */
    p->linked = GL_FALSE;
    for (uint32_t i = 0; i < p->attached_count; i++) {
        PSGLglShader *s = p->attached[i];
        if (!s || !s->compiled) continue;
        if (s->type == GL_VERTEX_SHADER) vertex = s->cg_program;
        if (s->type == GL_FRAGMENT_SHADER) fragment = s->cg_program;
    }
    if (!vertex || !fragment) {
        gls_set_log(p->log, sizeof(p->log), "missing compiled shader");
        return;
    }
    PSGLcgProgram *vp = psgl_cg_program(vertex);
    if (!vp || cellCgbGetVertexConfiguration(&vp->cgb_program, &conf) != CELL_CGB_OK) {
        gls_set_log(p->log, sizeof(p->log), "invalid vertex configuration"); return;
    }
    for (uint32_t hw = 0; hw < PSGL_MAX_GENERIC_ATTRIBS; ++hw) route[hw] = -1;
    for (uint32_t i = 0; i < vp->parameter_count; ++i) {
        PSGLcgParameter *param = &vp->parameters[i];
        if (param->direction != CG_IN || param->resource < CG_ATTR0 ||
            param->resource > CG_ATTR15) continue;
        uint32_t hw = (uint32_t)(param->resource - CG_ATTR0);
        if (!(conf.attributeInputMask & (1u << hw))) continue;
        if (count == PSGL_MAX_GL_ATTRIBS || !param->name[0] || (reflected & (1u << hw))) {
            gls_set_log(p->log, sizeof(p->log), "too many or ambiguous vertex inputs"); return;
        }
        reflected |= 1u << hw;
        gls_copy_name(linked[count].name, param->name, sizeof(linked[count].name));
        linked[count].index = PSGL_MAX_GL_ATTRIBS;
        hardware[count] = (uint8_t)hw;
        for (uint32_t j = 0; j < p->attrib_binding_count; ++j) {
            if (!gls_streq(p->attrib_bindings[j].name, param->name)) continue;
            uint32_t location = p->attrib_bindings[j].index;
            if (used & (1u << location)) {
                gls_set_log(p->log, sizeof(p->log), "active attributes alias a location"); return;
            }
            linked[count].index = location;
            used |= 1u << location;
            break;
        }
        ++count;
    }
    if (reflected != conf.attributeInputMask) {
        gls_set_log(p->log, sizeof(p->log), "vertex input reflection is incomplete"); return;
    }
    /* Binary metadata order is not part of GL's interface. Allocate unbound
       locations in ascending physical-input order for reproducible links. */
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t j = i;
        while (j && hardware[j] < hardware[j - 1]) {
            uint8_t hw = hardware[j];
            PSGLglAttribBinding binding = linked[j];
            hardware[j] = hardware[j - 1]; linked[j] = linked[j - 1];
            hardware[j - 1] = hw; linked[j - 1] = binding;
            --j;
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (linked[i].index == PSGL_MAX_GL_ATTRIBS) {
            uint32_t location = 0;
            while (used & (1u << location)) ++location;
            linked[i].index = location;
            used |= 1u << location;
        }
        route[hardware[i]] = (int8_t)linked[i].index;
    }
    p->vertex_program = vertex;
    p->fragment_program = fragment;
    p->uniform_count = 0;
    p->linked_attrib_count = count;
    gls_copy(p->linked_attribs, linked, count * sizeof(linked[0]));
    gls_copy(p->hw_to_location, route, sizeof(route));
    p->linked = GL_TRUE;
    gls_set_log(p->log, sizeof(p->log), "");
    if (g_current_program == program) glUseProgram(program);
}

GLAPI void glUseProgram(GLuint program)
{
    PSGLglProgram *p;
    if (program == 0u) {
        psgl_context_bind_cg_program(NULL, CG_PROFILE_SCE_VP_RSX);
        psgl_context_bind_cg_program(NULL, CG_PROFILE_SCE_FP_RSX);
        g_current_program = 0u;
        return;
    }
    p = gls_find_program(program);
    if (!p || !p->linked) { psgl_gl_error(GL_INVALID_OPERATION); return; }
    psgl_context_bind_cg_program(p->vertex_program, CG_PROFILE_SCE_VP_RSX);
    psgl_context_bind_cg_program(p->fragment_program, CG_PROFILE_SCE_FP_RSX);
    psgl_context_set_gl_attrib_routing(p->hw_to_location);
    g_current_program = program;
}

GLAPI void glGetProgramiv(GLuint program, GLenum pname, GLint *params)
{
    PSGLglProgram *p = gls_find_program(program);
    if (!params) return;
    *params = 0;
    if (!p) return;
    switch (pname) {
    case GL_DELETE_STATUS: *params = (GLint)p->deleted; break;
    case GL_LINK_STATUS: *params = (GLint)p->linked; break;
    case GL_VALIDATE_STATUS: *params = (GLint)p->linked; break;
    case GL_INFO_LOG_LENGTH: *params = (GLint)gls_len(p->log) + 1; break;
    case GL_ATTACHED_SHADERS: *params = (GLint)p->attached_count; break;
    case GL_ACTIVE_UNIFORMS: *params = (GLint)p->uniform_count; break;
    case GL_ACTIVE_UNIFORM_MAX_LENGTH: *params = 64; break;
    case GL_ACTIVE_ATTRIBUTES: *params = (GLint)p->linked_attrib_count; break;
    case GL_ACTIVE_ATTRIBUTE_MAX_LENGTH: *params = 64; break;
    default: break;
    }
}

GLAPI void glGetProgramInfoLog(GLuint program, GLsizei bufSize,
                               GLsizei *length, GLchar *infoLog)
{
    /* ES 2.0.25 section 6.1.8 p.131 restricts BOTH log commands when
     * SHADER_COMPILER is false, but not GetProgramiv(INFO_LOG_LENGTH). */
    (void)program; (void)bufSize; (void)length; (void)infoLog;
    psgl_gl_error(GL_INVALID_OPERATION);
}

GLAPI GLint glGetUniformLocation(GLuint program, const GLchar *name)
{
    PSGLglProgram *p = gls_find_program(program);
    PSGLglUniform *uniform;
    if (!p || !p->linked || !name) return -1;
    for (uint32_t i = 0; i < p->uniform_count; i++)
        if (gls_streq(p->uniforms[i].name, name))
            return gls_make_uniform_location(p, i);
    if (p->uniform_count >= PSGL_GL_MAX_UNIFORMS) return -1;
    uniform = &p->uniforms[p->uniform_count];
    uniform->vertex_parameter = gls_named_parameter(p->vertex_program, name);
    uniform->fragment_parameter = gls_named_parameter(p->fragment_program, name);
    if (!uniform->vertex_parameter && !uniform->fragment_parameter)
        return -1;
    gls_copy_name(uniform->name, name, sizeof(uniform->name));
    p->uniform_count++;
    return gls_make_uniform_location(p, p->uniform_count - 1u);
}

GLAPI GLint glGetAttribLocation(GLuint program, const GLchar *name)
{
    PSGLglProgram *p = gls_find_program(program);
    if (!p || !p->linked) { psgl_gl_error(GL_INVALID_OPERATION); return -1; }
    if (!name) return -1;
    for (uint32_t i = 0; i < p->linked_attrib_count; i++)
        if (gls_streq(p->linked_attribs[i].name, name))
            return (GLint)p->linked_attribs[i].index;
    return -1;
}

static void gls_uniform_set(GLint location, const GLfloat *values,
                            uint32_t count)
{
    PSGLglUniform *uniform = gls_find_uniform_by_location(location);
    PSGLcontext *context;
    if (!uniform || !values) return;
    if (uniform->vertex_parameter)
        psgl_cg_set_parameter_floats(psgl_cg_parameter(uniform->vertex_parameter),
                                     values, count);
    if (uniform->fragment_parameter)
        psgl_cg_set_parameter_floats(psgl_cg_parameter(uniform->fragment_parameter),
                                     values, count);
    context = psgl_context_current();
    if (context) context->dirty |= PSGL_DIRTY_CG | PSGL_DIRTY_TEXTURES;
}

GLAPI void glUniform1i(GLint location, GLint v0)
{
    GLfloat v[1] = {(GLfloat)v0};
    gls_uniform_set(location, v, 1u);
}

GLAPI void glUniform1f(GLint location, GLfloat v0)
{
    GLfloat v[1] = {v0};
    gls_uniform_set(location, v, 1u);
}

GLAPI void glUniform2f(GLint location, GLfloat v0, GLfloat v1)
{
    GLfloat v[2] = {v0, v1};
    gls_uniform_set(location, v, 2u);
}

GLAPI void glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2)
{
    GLfloat v[3] = {v0, v1, v2};
    gls_uniform_set(location, v, 3u);
}

GLAPI void glUniform4f(GLint location, GLfloat v0, GLfloat v1,
                       GLfloat v2, GLfloat v3)
{
    GLfloat v[4] = {v0, v1, v2, v3};
    gls_uniform_set(location, v, 4u);
}

GLAPI void glUniform1fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (count <= 0) return;
    gls_uniform_set(location, value, 1u);
}

GLAPI void glUniform2fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (count <= 0) return;
    gls_uniform_set(location, value, 2u);
}

GLAPI void glUniform3fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (count <= 0) return;
    gls_uniform_set(location, value, 3u);
}

GLAPI void glUniform4fv(GLint location, GLsizei count, const GLfloat *value)
{
    if (count <= 0) return;
    gls_uniform_set(location, value, 4u);
}

GLAPI void glUniformMatrix4fv(GLint location, GLsizei count,
                              GLboolean transpose, const GLfloat *value)
{
    (void)transpose;
    if (count <= 0) return;
    gls_uniform_set(location, value, 16u);
}

static PSGLcontext *gls_attrib_context(GLuint index)
{
    if (index >= PSGL_MAX_GL_ATTRIBS) {
        psgl_gl_error(GL_INVALID_VALUE);
        return NULL;
    }
    return psgl_context_current();
}

GLAPI void glEnableVertexAttribArray(GLuint index)
{
    PSGLcontext *context = gls_attrib_context(index);
    if (!context) return;
    context->gl_attribs[index].enabled = GL_TRUE;
    /* Desktop fixed-function attribute zero aliases the vertex array.
       GL shader-object locations never alias the fixed-function arrays. */
    if (!index && !context->bound_vertex_program)
        context->attribs[PSGL_ATTRIB_VERTEX].enabled = GL_TRUE;
    context->dirty |= PSGL_DIRTY_CG;
}

GLAPI void glDisableVertexAttribArray(GLuint index)
{
    PSGLcontext *context = gls_attrib_context(index);
    if (!context) return;
    context->gl_attribs[index].enabled = GL_FALSE;
    if (!index && !context->bound_vertex_program)
        context->attribs[PSGL_ATTRIB_VERTEX].enabled = GL_FALSE;
    context->dirty |= PSGL_DIRTY_CG;
}

GLAPI void glVertexAttribPointer(GLuint index, GLint size, GLenum type,
                                 GLboolean normalized, GLsizei stride,
                                 const GLvoid *pointer)
{
    PSGLcontext *context = gls_attrib_context(index);
    (void)normalized; /* Normalization has a separate regression increment. */
    if (!context) return;
    if (size < 1 || size > 4 || stride < 0) {
        psgl_gl_error(GL_INVALID_VALUE); return;
    }
    if (type != GL_FLOAT && type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT) {
        psgl_gl_error(GL_INVALID_ENUM); return;
    }
    if (stride > 255) { psgl_gl_error(GL_INVALID_OPERATION); return; }
    if (context->bound_array_buffer && (uintptr_t)pointer > UINT32_MAX) {
        psgl_gl_error(GL_INVALID_OPERATION); return;
    }
    PSGLvertexAttribState *attrib = &context->gl_attribs[index];
    attrib->size = size;
    attrib->type = type;
    attrib->stride = stride;
    attrib->pointer = pointer;
    attrib->buffer_name = context->bound_array_buffer;
    attrib->buffer_offset = context->bound_array_buffer ? (uint32_t)(uintptr_t)pointer : 0;
    if (!index && !context->bound_vertex_program)
        context->attribs[PSGL_ATTRIB_VERTEX] = *attrib;
    context->dirty |= PSGL_DIRTY_CG;
}

GLAPI void glVertexAttrib4f(GLuint index, GLfloat x, GLfloat y,
                            GLfloat z, GLfloat w)
{
    PSGLcontext *context = gls_attrib_context(index);
    if (!context) return;
    GLfloat *current = context->gl_current_attribs[index];
    current[0] = x; current[1] = y; current[2] = z; current[3] = w;
    context->dirty |= PSGL_DIRTY_CG;
}
