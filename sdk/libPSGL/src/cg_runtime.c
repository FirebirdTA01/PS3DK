#include "cg_internal.h"
#include <cell/cgb/cgb_levelc.h>

#include <malloc.h>
#include <ppu-types.h>
#include <stddef.h>

#define PSGL_CELL_FS_O_RDONLY 000000
#define PSGL_CELL_FS_SEEK_SET 0
#define PSGL_CELL_FS_SEEK_END 2

extern s32 cellFsOpen(const char *path, s32 flags, s32 *fd,
                      const void *arg, u64 argsize);
extern s32 cellFsClose(s32 fd);
extern s32 cellFsRead(s32 fd, void *ptr, u64 len, u64 *read);
extern s32 cellFsLseek(s32 fd, s64 offset, s32 whence, u64 *position);

static CGerror g_cg_error = CG_NO_ERROR;

static void cg_zero(void *ptr, size_t size)
{
    unsigned char *out = (unsigned char *)ptr;
    while (size--) {
        *out++ = 0;
    }
}

static void cg_copy(void *dst, const void *src, size_t size)
{
    unsigned char *out = (unsigned char *)dst;
    const unsigned char *in = (const unsigned char *)src;
    while (size--) {
        *out++ = *in++;
    }
}

static int cg_streq(const char *a, const char *b)
{
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int cg_array_element_of(const char *element, const char *base)
{
    if (!element || !base) return 0;
    while (*base && *element == *base) {
        element++;
        base++;
    }
    return *base == '\0' && *element == '[';
}

static void cg_copy_name(char *dst, const char *src)
{
    uint32_t i = 0;
    if (!dst) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (; i + 1u < PSGL_CG_NAME_MAX && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

PSGLcgContext *psgl_cg_context(CGcontext ctx)
{
    PSGLcgContext *context = (PSGLcgContext *)ctx;
    return (context && context->magic == PSGL_CG_CONTEXT_MAGIC) ?
        context : NULL;
}

PSGLcgProgram *psgl_cg_program(CGprogram program)
{
    PSGLcgProgram *p = (PSGLcgProgram *)program;
    return (p && p->magic == PSGL_CG_PROGRAM_MAGIC) ? p : NULL;
}

PSGLcgParameter *psgl_cg_parameter(CGparameter parameter)
{
    PSGLcgParameter *p = (PSGLcgParameter *)parameter;
    return (p && p->magic == PSGL_CG_PARAMETER_MAGIC) ? p : NULL;
}

void psgl_cg_set_error(PSGLcgContext *context, CGerror error)
{
    g_cg_error = error;
    if (context) context->last_error = error;
}

int psgl_cg_profile_is_vertex(CGprofile profile)
{
    return profile == CG_PROFILE_SCE_VP_RSX ||
           profile == CG_PROFILE_VPRSX ||
           profile == CG_PROFILE_VP40 ||
           profile == CG_PROFILE_ARBVP1 ||
           profile == CG_PROFILE_VP30 ||
           profile == CG_PROFILE_VP20;
}

int psgl_cg_profile_is_fragment(CGprofile profile)
{
    return profile == CG_PROFILE_SCE_FP_RSX ||
           profile == CG_PROFILE_FPRSX ||
           profile == CG_PROFILE_FP40 ||
           profile == CG_PROFILE_ARBFP1 ||
           profile == CG_PROFILE_FP30 ||
           profile == CG_PROFILE_FP20;
}

static CGprofile cg_default_profile(const CgBinaryProgram *binary,
                                    CGprofile requested)
{
    if (requested != CG_PROFILE_UNKNOWN && requested != (CGprofile)0)
        return requested;
    if (!binary) return CG_PROFILE_UNKNOWN;
    return binary->profile;
}

static void cg_link_program(PSGLcgContext *context, PSGLcgProgram *program)
{
    program->next = context->programs;
    if (context->programs) context->programs->prev = program;
    context->programs = program;
}

static void cg_unlink_program(PSGLcgProgram *program)
{
    if (!program || !program->context) return;
    if (program->prev) program->prev->next = program->next;
    if (program->next) program->next->prev = program->prev;
    if (program->context->programs == program)
        program->context->programs = program->next;
    program->context = NULL;
    program->next = NULL;
    program->prev = NULL;
}

static void cg_free_program(PSGLcgProgram *program)
{
    if (!program) return;
    cg_unlink_program(program);
    if (program->parameters) free(program->parameters);
    if (program->binary) free(program->binary);
    program->magic = 0u;
    free(program);
}

static void *cg_read_file(const char *path, uint32_t *out_size)
{
    s32 fd = -1;
    void *data;
    u64 end = 0u;
    u64 pos = 0u;
    u64 got = 0u;

    if (out_size) *out_size = 0u;
    if (!path || !out_size) return NULL;
    if (cellFsOpen(path, PSGL_CELL_FS_O_RDONLY, &fd, NULL, 0) != 0)
        return NULL;
    if (cellFsLseek(fd, 0, PSGL_CELL_FS_SEEK_END, &end) != 0 ||
        end == 0u || end > 0xffffffffULL ||
        cellFsLseek(fd, 0, PSGL_CELL_FS_SEEK_SET, &pos) != 0) {
        cellFsClose(fd);
        return NULL;
    }

    data = malloc((size_t)end);
    if (!data) {
        cellFsClose(fd);
        return NULL;
    }
    if (cellFsRead(fd, data, end, &got) != 0 || got != end) {
        cellFsClose(fd);
        free(data);
        return NULL;
    }
    cellFsClose(fd);
    *out_size = (uint32_t)got;
    return data;
}

static void cg_init_parameter(PSGLcgProgram *program, uint32_t index,
                              PSGLcgParameter *parameter)
{
    const float *defaults = NULL;

    parameter->magic = PSGL_CG_PARAMETER_MAGIC;
    parameter->program = program;
    parameter->index = index;
    parameter->vertex_register = 0xffffu;
    parameter->fragment_register = 0xffffu;

    {
        const unsigned char *b = (const unsigned char *)program->binary;
        int is_compact = b && b[0] == 0x43 && b[1] == 0x47 &&
                         b[2] == 0x42 && b[3] == 0x00;
        if (is_compact) {
            /* Compact CGB: use LevelC metadata when present, fall
             * back to LevelB resource decode when there is no
             * LevelC (content=0x03, NormalMap / basic_psgl). */
            uint32_t name_len = (uint32_t)sizeof(parameter->name);
            uint16_t cg_type;
            uint16_t cg_resource;
            uint16_t cg_var;
            uint16_t cg_dir;

            cellCgbMapGetName(&program->cgb_program, index,
                              parameter->name, &name_len);

            cg_type     = cellCgbLevelCMapGetCgType(&program->cgb_program, index);
            cg_resource = cellCgbLevelCMapGetCgResource(&program->cgb_program, index);
            cg_var      = cellCgbLevelCMapGetVariability(&program->cgb_program, index);
            cg_dir      = cellCgbLevelCMapGetDirection(&program->cgb_program, index);

            if (cg_type) {
                parameter->type         = (CGtype)cg_type;
                parameter->resource     = (CGresource)cg_resource;
                parameter->variability  = (CGenum)cg_var;
                parameter->direction    = (CGenum)cg_dir;
                /* Decode resource_index from LevelB value in a way
                 * that matches what the raw CgBinary path provides. */
                {
                    uint16_t value = cellCgbMapGetValue(
                        &program->cgb_program, index);
                    if (cg_resource == CG_C && program->cgb_profile ==
                        CELL_CGB_PROFILE_VERTEX)
                        parameter->resource_index = (int32_t)(value - 0x0100u);
                    else if (cg_resource >= CG_TEXUNIT0 &&
                             cg_resource <= CG_TEXUNIT15)
                        parameter->resource_index = (int32_t)cg_resource;
                    else
                        parameter->resource_index = (int32_t)value;
                }
            } else {
                /* No LevelC — decode resource from LevelB. */
                uint16_t value = cellCgbMapGetValue(
                    &program->cgb_program, index);
                if (program->cgb_profile == CELL_CGB_PROFILE_VERTEX) {
                    if (value >= 0x0100u) {
                        parameter->resource = CG_C;
                        parameter->resource_index = (int32_t)(value - 0x0100u);
                    } else {
                        parameter->resource = (CGresource)(CG_ATTR0 + (int32_t)value);
                        parameter->resource_index = (int32_t)value;
                    }
                } else {
                    if (value < 1024u) {
                        parameter->resource = (CGresource)(CG_TEXUNIT0 + (int32_t)value);
                        parameter->resource_index = (int32_t)(CG_TEXUNIT0 + (int32_t)value);
                    } else {
                        parameter->resource = CG_C;
                        parameter->resource_index = 0;
                    }
                }
                parameter->type         = CG_FLOAT4;
                parameter->variability  = CG_UNIFORM;
                parameter->direction    = CG_IN;
            }
        } else {
        /* Raw CgBinaryProgram path (legacy, retire after -mcgb compact emit). */
        const CgBinaryProgram *binary = (const CgBinaryProgram *)program->binary;
        const CgBinaryParameter *binary_param =
            (const CgBinaryParameter *)((const unsigned char *)binary +
                                        binary->parameterArray) + index;
        const char *name = binary_param->name ?
            (const char *)binary + binary_param->name : NULL;

        parameter->type = binary_param->type;
        parameter->resource = binary_param->res;
        parameter->variability = binary_param->var;
        parameter->direction = binary_param->direction;
        parameter->resource_index =
            (binary_param->res >= CG_TEXUNIT0 &&
             binary_param->res <= CG_TEXUNIT15) ?
            (int32_t)binary_param->res : binary_param->resIndex;
        cg_copy_name(parameter->name, name);
    }
    }

    cellCgbMapGetVertexUniformRegister(&program->cgb_program, index,
                                       &parameter->vertex_register,
                                       &defaults);
    cellCgbMapGetFragmentUniformRegister(&program->cgb_program, index,
                                         &parameter->fragment_register);
    if (defaults) {
        cg_copy(parameter->value, defaults, 4u * sizeof(float));
        parameter->value_count = 4u;
    }
}

static int cg_build_parameters(PSGLcgProgram *program)
{
    uint32_t count = cellCgbMapGetLength(&program->cgb_program);
    program->parameter_count = count;
    if (count == 0u) return 1;
    program->parameters =
        (PSGLcgParameter *)calloc(count, sizeof(PSGLcgParameter));
    if (!program->parameters) return 0;
    for (uint32_t i = 0; i < count; i++)
        cg_init_parameter(program, i, &program->parameters[i]);
    return 1;
}

static CGprogram cg_create_binary_program(PSGLcgContext *context,
                                          CgBinaryProgram *binary,
                                          uint32_t binary_size,
                                          CGprofile profile)
{
    PSGLcgProgram *program;
    CGprofile selected;
    CellCgbProfile cgb_profile;

    selected = cg_default_profile(binary, profile);
    if (!psgl_cg_profile_is_vertex(selected) &&
        !psgl_cg_profile_is_fragment(selected)) {
        free(binary);
        psgl_cg_set_error(context, CG_INVALID_PROFILE_ERROR);
        return NULL;
    }

    program = (PSGLcgProgram *)calloc(1u, sizeof(PSGLcgProgram));
    if (!program) {
        free(binary);
        psgl_cg_set_error(context, CG_MEMORY_ALLOC_ERROR);
        return NULL;
    }
    program->magic = PSGL_CG_PROGRAM_MAGIC;
    program->context = context;
    program->profile = selected;
    program->binary = binary;
    program->binary_size = binary_size;

    if (cellCgbRead(binary, binary_size, &program->cgb_program) != CELL_CGB_OK) {
        cg_free_program(program);
        psgl_cg_set_error(context, CG_PROGRAM_LOAD_ERROR);
        return NULL;
    }
    cgb_profile = cellCgbGetProfile(&program->cgb_program);
    if ((psgl_cg_profile_is_vertex(selected) &&
         cgb_profile != CELL_CGB_PROFILE_VERTEX) ||
        (psgl_cg_profile_is_fragment(selected) &&
         cgb_profile != CELL_CGB_PROFILE_FRAGMENT)) {
        cg_free_program(program);
        psgl_cg_set_error(context, CG_INVALID_PROFILE_ERROR);
        return NULL;
    }
    program->cgb_profile = cgb_profile;
    if (!cg_build_parameters(program)) {
        cg_free_program(program);
        psgl_cg_set_error(context, CG_MEMORY_ALLOC_ERROR);
        return NULL;
    }

    cg_link_program(context, program);
    psgl_cg_set_error(context, CG_NO_ERROR);
    return (CGprogram)program;
}

CGprogram psgl_cg_create_program_from_memory(CGcontext ctx,
                                             const void *data,
                                             uint32_t size,
                                             CGprofile profile)
{
    PSGLcgContext *context = psgl_cg_context(ctx);
    CgBinaryProgram *binary;
    if (!context) {
        psgl_cg_set_error(NULL, CG_INVALID_CONTEXT_HANDLE_ERROR);
        return NULL;
    }
    if (!data || !size) {
        psgl_cg_set_error(context, CG_INVALID_PARAMETER_ERROR);
        return NULL;
    }
    binary = (CgBinaryProgram *)malloc(size);
    if (!binary) {
        psgl_cg_set_error(context, CG_MEMORY_ALLOC_ERROR);
        return NULL;
    }
    cg_copy(binary, data, size);
    return cg_create_binary_program(context, binary, size, profile);
}

static CGparameterclass cg_class_for_type(CGtype type)
{
    if (type >= CG_FLOAT1x1 && type <= CG_FLOAT4x4)
        return CG_PARAMETERCLASS_MATRIX;
    if (type == CG_FLOAT || type == CG_INT || type == CG_BOOL)
        return CG_PARAMETERCLASS_SCALAR;
    if ((type >= CG_FLOAT1 && type <= CG_FLOAT4) ||
        (type >= CG_INT1 && type <= CG_INT4) ||
        (type >= CG_BOOL1 && type <= CG_BOOL4))
        return CG_PARAMETERCLASS_VECTOR;
    return CG_PARAMETERCLASS_UNKNOWN;
}

static int cg_columns_for_type(CGtype type)
{
    if (type == CG_FLOAT || type == CG_INT || type == CG_BOOL) return 1;
    if (type == CG_FLOAT1 || type == CG_INT1 || type == CG_BOOL1) return 1;
    if (type == CG_FLOAT2 || type == CG_INT2 || type == CG_BOOL2) return 2;
    if (type == CG_FLOAT3 || type == CG_INT3 || type == CG_BOOL3) return 3;
    if (type == CG_FLOAT4 || type == CG_INT4 || type == CG_BOOL4) return 4;
    if (type >= CG_FLOAT1x1 && type <= CG_FLOAT1x4)
        return (int)(type - CG_FLOAT1x1) + 1;
    if (type >= CG_FLOAT2x1 && type <= CG_FLOAT2x4)
        return (int)(type - CG_FLOAT2x1) + 1;
    if (type >= CG_FLOAT3x1 && type <= CG_FLOAT3x4)
        return (int)(type - CG_FLOAT3x1) + 1;
    if (type >= CG_FLOAT4x1 && type <= CG_FLOAT4x4)
        return (int)(type - CG_FLOAT4x1) + 1;
    return 0;
}

static int cg_rows_for_type(CGtype type)
{
    if (type >= CG_FLOAT1x1 && type <= CG_FLOAT1x4) return 1;
    if (type >= CG_FLOAT2x1 && type <= CG_FLOAT2x4) return 2;
    if (type >= CG_FLOAT3x1 && type <= CG_FLOAT3x4) return 3;
    if (type >= CG_FLOAT4x1 && type <= CG_FLOAT4x4) return 4;
    return cg_columns_for_type(type) ? 1 : 0;
}

void psgl_cg_set_parameter_floats(PSGLcgParameter *parameter,
                                  const float *values, uint32_t count)
{
    if (!parameter || !values) return;
    if (count > 16u) count = 16u;
    cg_copy(parameter->value, values, count * sizeof(float));
    parameter->value_count = count;
    parameter->dirty = CG_TRUE;
}

void psgl_cg_get_parameter_floats(PSGLcgParameter *parameter,
                                  float *values, uint32_t count)
{
    if (!values) return;
    for (uint32_t i = 0; i < count; i++)
        values[i] = (parameter && i < parameter->value_count) ?
            parameter->value[i] : 0.0f;
}

void psgl_cg_set_parameter_matrix(PSGLcgParameter *parameter,
                                  const float *values)
{
    psgl_cg_set_parameter_floats(parameter, values, 16u);
}

void psgl_cg_get_parameter_matrix(PSGLcgParameter *parameter,
                                  float *values)
{
    psgl_cg_get_parameter_floats(parameter, values, 16u);
}

CG_API CGerror CGENTRY cgGetError(void)
{
    CGerror e = g_cg_error;
    g_cg_error = CG_NO_ERROR;
    return e;
}

CG_API CGerror CGENTRY cgGetFirstError(void)
{
    CGerror e = g_cg_error;
    g_cg_error = CG_NO_ERROR;
    return e;
}

CG_API const char *CGENTRY cgGetErrorString(CGerror error)
{
    (void)error;
    return "no error";
}

CG_API const char *CGENTRY cgGetLastErrorString(CGerror *error)
{
    if (error) *error = g_cg_error;
    g_cg_error = CG_NO_ERROR;
    return "no error";
}

CG_API void CGENTRY cgSetErrorCallback(CGerrorCallbackFunc func)
{ (void)func; }

CG_API CGerrorCallbackFunc CGENTRY cgGetErrorCallback(void) { return NULL; }

CG_API void CGENTRY cgSetErrorHandler(CGerrorHandlerFunc func, void *data)
{ (void)func; (void)data; }

CG_API CGerrorHandlerFunc CGENTRY cgGetErrorHandler(void **data)
{ if (data) *data = NULL; return NULL; }

/* ── string ──────────────────────────────────────────────────────── */

CG_API const char *CGENTRY cgGetString(CGenum sname)
{ (void)sname; return "PS3DK Cg Stub"; }

/* ── locking / semantic policy ───────────────────────────────────── */

CG_API CGenum CGENTRY cgSetLockingPolicy(CGenum lockingPolicy)
{ (void)lockingPolicy; return 0; }

CG_API CGenum CGENTRY cgGetLockingPolicy(void) { return 0; }

CG_API CGenum CGENTRY cgSetSemanticCasePolicy(CGenum casePolicy)
{ (void)casePolicy; return 0; }

CG_API CGenum CGENTRY cgGetSemanticCasePolicy(void) { return 0; }

/* ── context ─────────────────────────────────────────────────────── */

CG_API CGcontext CGENTRY cgCreateContext(void)
{
    PSGLcgContext *context =
        (PSGLcgContext *)calloc(1u, sizeof(PSGLcgContext));
    if (!context) {
        psgl_cg_set_error(NULL, CG_MEMORY_ALLOC_ERROR);
        return NULL;
    }
    context->magic = PSGL_CG_CONTEXT_MAGIC;
    context->last_error = CG_NO_ERROR;
    return (CGcontext)context;
}

CG_API void CGENTRY cgDestroyContext(CGcontext ctx)
{
    PSGLcgContext *context = psgl_cg_context(ctx);
    if (!context) return;
    while (context->programs)
        cg_free_program(context->programs);
    context->magic = 0u;
    free(context);
}

CG_API CGbool CGENTRY cgIsContext(CGcontext ctx)
{ return psgl_cg_context(ctx) ? CG_TRUE : CG_FALSE; }

CG_API const char *CGENTRY cgGetLastListing(CGcontext ctx)
{ (void)ctx; return NULL; }

CG_API void CGENTRY cgSetLastListing(CGhandle handle, const char *listing)
{ (void)handle; (void)listing; }

CG_API void CGENTRY cgSetAutoCompile(CGcontext ctx, CGenum flag)
{ (void)ctx; (void)flag; }

CG_API CGenum CGENTRY cgGetAutoCompile(CGcontext ctx)
{ (void)ctx; return 0; }

CG_API void CGENTRY cgSetParameterSettingMode(CGcontext ctx,
                                              CGenum parameterSettingMode)
{ (void)ctx; (void)parameterSettingMode; }

CG_API CGenum CGENTRY cgGetParameterSettingMode(CGcontext ctx)
{ (void)ctx; return 0; }

/* ── program ─────────────────────────────────────────────────────── */

CG_API CGprogram CGENTRY cgCreateProgram(CGcontext ctx, CGenum program_type,
                                         const char *program,
                                         CGprofile profile,
                                         const char *entry,
                                         const char **args)
{ (void)ctx; (void)program_type; (void)program;
  (void)profile; (void)entry; (void)args; return NULL; }

CG_API CGprogram CGENTRY cgCreateProgramFromFile(CGcontext ctx,
                                                 CGenum program_type,
                                                 const char *program_file,
                                                 CGprofile profile,
                                                 const char *entry,
                                                 const char **args)
{
    PSGLcgContext *context = psgl_cg_context(ctx);
    CgBinaryProgram *binary;
    uint32_t binary_size = 0u;

    (void)entry;
    (void)args;
    if (!context) {
        psgl_cg_set_error(NULL, CG_INVALID_CONTEXT_HANDLE_ERROR);
        return NULL;
    }
    if (program_type != CG_BINARY || !program_file) {
        psgl_cg_set_error(context, CG_INVALID_ENUMERANT_ERROR);
        return NULL;
    }

    binary = (CgBinaryProgram *)cg_read_file(program_file, &binary_size);
    if (!binary) {
        psgl_cg_set_error(context, CG_FILE_READ_ERROR);
        return NULL;
    }
    return cg_create_binary_program(context, binary, binary_size, profile);
}

CG_API CGprogram CGENTRY cgCopyProgram(CGprogram program)
{
    (void)program;
    g_cg_error = CG_INVALID_PARAMETER_HANDLE_ERROR;
    return NULL;
}

CG_API void CGENTRY cgDestroyProgram(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    if (p) cg_free_program(p);
}

CG_API CGprogram CGENTRY cgGetFirstProgram(CGcontext ctx)
{
    PSGLcgContext *context = psgl_cg_context(ctx);
    return context ? (CGprogram)context->programs : NULL;
}

CG_API CGprogram CGENTRY cgGetNextProgram(CGprogram current)
{
    PSGLcgProgram *program = psgl_cg_program(current);
    return program ? (CGprogram)program->next : NULL;
}

CG_API CGcontext CGENTRY cgGetProgramContext(CGprogram prog)
{
    PSGLcgProgram *program = psgl_cg_program(prog);
    return program ? (CGcontext)program->context : NULL;
}

CG_API CGbool CGENTRY cgIsProgram(CGprogram program)
{ return psgl_cg_program(program) ? CG_TRUE : CG_FALSE; }

CG_API void CGENTRY cgCompileProgram(CGprogram program) { (void)program; }

CG_API CGbool CGENTRY cgIsProgramCompiled(CGprogram program)
{ return psgl_cg_program(program) ? CG_TRUE : CG_FALSE; }

CG_API const char *CGENTRY cgGetProgramString(CGprogram prog, CGenum pname)
{
    PSGLcgProgram *program = psgl_cg_program(prog);
    if (!program) return NULL;
    if (pname == CG_COMPILED_PROGRAM || pname == CG_PROGRAM_SOURCE)
        return (const char *)program->binary;
    return NULL;
}

CG_API CGprofile CGENTRY cgGetProgramProfile(CGprogram prog)
{
    PSGLcgProgram *program = psgl_cg_program(prog);
    return program ? program->profile : (CGprofile)0;
}

CG_API char const *const *CGENTRY cgGetProgramOptions(CGprogram prog)
{ (void)prog; return NULL; }

CG_API void CGENTRY cgSetProgramProfile(CGprogram prog, CGprofile profile)
{
    PSGLcgProgram *program = psgl_cg_program(prog);
    if (program && (psgl_cg_profile_is_vertex(profile) ||
                    psgl_cg_profile_is_fragment(profile)))
        program->profile = profile;
}

CG_API CGenum CGENTRY cgGetProgramInput(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    if (!p) return 0;
    return psgl_cg_profile_is_vertex(p->profile) ? CG_VERTEX : CG_FRAGMENT;
}

CG_API CGenum CGENTRY cgGetProgramOutput(CGprogram program)
{
    PSGLcgProgram *p = psgl_cg_program(program);
    if (!p) return 0;
    return psgl_cg_profile_is_vertex(p->profile) ? CG_VERTEX : CG_FRAGMENT;
}

CG_API void CGENTRY cgSetPassProgramParameters(CGprogram p) { (void)p; }
CG_API void CGENTRY cgUpdateProgramParameters(CGprogram p)  { (void)p; }

/* ── parameter ───────────────────────────────────────────────────── */

CG_API CGparameter CGENTRY cgCreateParameter(CGcontext ctx, CGtype type)
{ (void)ctx; (void)type; return NULL; }

CG_API CGparameter CGENTRY cgCreateParameterArray(CGcontext ctx,
                                                  CGtype type, int length)
{ (void)ctx; (void)type; (void)length; return NULL; }

CG_API CGparameter CGENTRY cgCreateParameterMultiDimArray(CGcontext ctx,
    CGtype type, int dim, const int *lengths)
{ (void)ctx; (void)type; (void)dim; (void)lengths; return NULL; }

CG_API void CGENTRY cgDestroyParameter(CGparameter param) { (void)param; }
CG_API void CGENTRY cgConnectParameter(CGparameter from, CGparameter to)
{ (void)from; (void)to; }
CG_API void CGENTRY cgDisconnectParameter(CGparameter param) { (void)param; }
CG_API CGparameter CGENTRY cgGetConnectedParameter(CGparameter param)
{ (void)param; return NULL; }
CG_API int CGENTRY cgGetNumConnectedToParameters(CGparameter param)
{ (void)param; return 0; }
CG_API CGparameter CGENTRY cgGetConnectedToParameter(CGparameter param,
                                                     int index)
{ (void)param; (void)index; return NULL; }

CG_API CGparameter CGENTRY cgGetNamedParameter(CGprogram prog,
                                               const char *name)
{
    PSGLcgProgram *program = psgl_cg_program(prog);
    if (!program || !name) return NULL;
    for (uint32_t i = 0; i < program->parameter_count; i++) {
        if (cg_streq(program->parameters[i].name, name))
            return (CGparameter)&program->parameters[i];
    }
    for (uint32_t i = 0; i < program->parameter_count; i++) {
        if (cg_array_element_of(program->parameters[i].name, name))
            return (CGparameter)&program->parameters[i];
    }
    return NULL;
}

CG_API CGparameter CGENTRY cgGetNamedProgramParameter(CGprogram prog,
    CGenum name_space, const char *name)
{ (void)name_space; return cgGetNamedParameter(prog, name); }

CG_API CGparameter CGENTRY cgGetFirstParameter(CGprogram prog,
                                               CGenum name_space)
{
    PSGLcgProgram *program = psgl_cg_program(prog);
    (void)name_space;
    return (program && program->parameter_count) ?
        (CGparameter)&program->parameters[0] : NULL;
}

CG_API CGparameter CGENTRY cgGetNextParameter(CGparameter current)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(current);
    PSGLcgProgram *program = parameter ? parameter->program : NULL;
    if (!program || parameter->index + 1u >= program->parameter_count)
        return NULL;
    return (CGparameter)&program->parameters[parameter->index + 1u];
}

CG_API CGparameter CGENTRY cgGetFirstLeafParameter(CGprogram prog,
                                                   CGenum name_space)
{ return cgGetFirstParameter(prog, name_space); }

CG_API CGparameter CGENTRY cgGetNextLeafParameter(CGparameter current)
{ return cgGetNextParameter(current); }

CG_API CGparameter CGENTRY cgGetFirstStructParameter(CGparameter param)
{ (void)param; return NULL; }

CG_API CGparameter CGENTRY cgGetNamedStructParameter(CGparameter param,
                                                     const char *name)
{ (void)param; (void)name; return NULL; }

CG_API CGparameter CGENTRY cgGetFirstDependentParameter(CGparameter param)
{ (void)param; return NULL; }

CG_API CGparameter CGENTRY cgGetArrayParameter(CGparameter aparam, int index)
{ (void)aparam; (void)index; return NULL; }

CG_API int CGENTRY cgGetArrayDimension(CGparameter param)
{ (void)param; return 0; }
CG_API CGtype CGENTRY cgGetArrayType(CGparameter param)
{ (void)param; return (CGtype)0; }
CG_API int CGENTRY cgGetArraySize(CGparameter param, int dimension)
{ (void)param; (void)dimension; return 0; }
CG_API int CGENTRY cgGetArrayTotalSize(CGparameter param)
{ (void)param; return 0; }
CG_API void CGENTRY cgSetArraySize(CGparameter param, int size)
{ (void)param; (void)size; }
CG_API void CGENTRY cgSetMultiDimArraySize(CGparameter param,
                                           const int *sizes)
{ (void)param; (void)sizes; }

CG_API CGprogram CGENTRY cgGetParameterProgram(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? (CGprogram)parameter->program : NULL;
}
CG_API CGcontext CGENTRY cgGetParameterContext(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return (parameter && parameter->program) ?
        (CGcontext)parameter->program->context : NULL;
}
CG_API CGbool CGENTRY cgIsParameter(CGparameter param)
{ return psgl_cg_parameter(param) ? CG_TRUE : CG_FALSE; }
CG_API const char *CGENTRY cgGetParameterName(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? parameter->name : NULL;
}
CG_API CGtype CGENTRY cgGetParameterType(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? parameter->type : (CGtype)0;
}
CG_API CGtype CGENTRY cgGetParameterBaseType(CGparameter param)
{ return cgGetParameterType(param); }
CG_API CGparameterclass CGENTRY cgGetParameterClass(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? cg_class_for_type(parameter->type) :
        CG_PARAMETERCLASS_UNKNOWN;
}
CG_API int CGENTRY cgGetParameterRows(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? cg_rows_for_type(parameter->type) : 0;
}
CG_API int CGENTRY cgGetParameterColumns(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? cg_columns_for_type(parameter->type) : 0;
}
CG_API CGtype CGENTRY cgGetParameterNamedType(CGparameter param)
{ (void)param; return (CGtype)0; }
CG_API const char *CGENTRY cgGetParameterSemantic(CGparameter param)
{ (void)param; return NULL; }
CG_API CGresource CGENTRY cgGetParameterResource(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? parameter->resource : (CGresource)0;
}
CG_API CGresource CGENTRY cgGetParameterBaseResource(CGparameter param)
{ return cgGetParameterResource(param); }
CG_API unsigned long CGENTRY cgGetParameterResourceIndex(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? (unsigned long)parameter->resource_index : 0UL;
}
CG_API CGenum CGENTRY cgGetParameterVariability(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? parameter->variability : 0;
}
CG_API CGenum CGENTRY cgGetParameterDirection(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? parameter->direction : 0;
}
CG_API CGbool CGENTRY cgIsParameterReferenced(CGparameter param)
{ return psgl_cg_parameter(param) ? CG_TRUE : CG_FALSE; }
CG_API CGbool CGENTRY cgIsParameterUsed(CGparameter param, CGhandle handle)
{ (void)param; (void)handle; return CG_FALSE; }

CG_API const double *CGENTRY cgGetParameterValues(CGparameter param,
    CGenum value_type, int *nvalues)
{ (void)param; (void)value_type;
  if (nvalues) *nvalues = 0;
  return NULL; }

/* parameter set/get: mostly NOPs */
CG_API void CGENTRY cgSetParameterValuedr(CGparameter p, int n, const double *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValuedc(CGparameter p, int n, const double *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValuefr(CGparameter p, int n, const float *v)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(p);
    if (parameter && n > 0)
        psgl_cg_set_parameter_floats(parameter, v, (uint32_t)n);
}
CG_API void CGENTRY cgSetParameterValuefc(CGparameter p, int n, const float *v)
{ cgSetParameterValuefr(p, n, v); }
CG_API void CGENTRY cgSetParameterValueir(CGparameter p, int n, const int *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValueic(CGparameter p, int n, const int *v)
{ (void)p; (void)n; (void)v; }
CG_API int CGENTRY cgGetParameterValuedr(CGparameter p, int n, double *v)
{ (void)p; (void)n; if (v) cg_zero(v, (size_t)n * sizeof(double)); return 0; }
CG_API int CGENTRY cgGetParameterValuedc(CGparameter p, int n, double *v)
{ (void)p; (void)n; if (v) cg_zero(v, (size_t)n * sizeof(double)); return 0; }
CG_API int CGENTRY cgGetParameterValuefr(CGparameter p, int n, float *v)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(p);
    if (n < 0) return 0;
    psgl_cg_get_parameter_floats(parameter, v, (uint32_t)n);
    return parameter ? n : 0;
}
CG_API int CGENTRY cgGetParameterValuefc(CGparameter p, int n, float *v)
{ return cgGetParameterValuefr(p, n, v); }
CG_API int CGENTRY cgGetParameterValueir(CGparameter p, int n, int *v)
{ (void)p; (void)n; if (v) cg_zero(v, (size_t)n * sizeof(int)); return 0; }
CG_API int CGENTRY cgGetParameterValueic(CGparameter p, int n, int *v)
{ (void)p; (void)n; if (v) cg_zero(v, (size_t)n * sizeof(int)); return 0; }

CG_API const char *CGENTRY cgGetStringParameterValue(CGparameter param)
{ (void)param; return NULL; }
CG_API void CGENTRY cgSetStringParameterValue(CGparameter param,
                                              const char *str)
{ (void)param; (void)str; }

CG_API int CGENTRY cgGetParameterOrdinalNumber(CGparameter param)
{ (void)param; return -1; }
CG_API CGbool CGENTRY cgIsParameterGlobal(CGparameter param)
{ (void)param; return CG_FALSE; }
CG_API int CGENTRY cgGetParameterIndex(CGparameter param)
{
    PSGLcgParameter *parameter = psgl_cg_parameter(param);
    return parameter ? (int)parameter->index : -1;
}
CG_API void CGENTRY cgSetParameterVariability(CGparameter param, CGenum vary)
{ (void)param; (void)vary; }
CG_API void CGENTRY cgSetParameterSemantic(CGparameter param,
                                           const char *semantic)
{ (void)param; (void)semantic; }

/* scalar parameter setters: int variants only.
 * float/double variants are macro-aliased to cgGL* forms in <Cg/cg.h>
 * and are defined in cggl_bridge.c. */
CG_API void CGENTRY cgSetParameter1i(CGparameter p, int x)
{ (void)p; (void)x; }
CG_API void CGENTRY cgSetParameter2i(CGparameter p, int x, int y)
{ (void)p; (void)x; (void)y; }
CG_API void CGENTRY cgSetParameter3i(CGparameter p, int x, int y, int z)
{ (void)p; (void)x; (void)y; (void)z; }
CG_API void CGENTRY cgSetParameter4i(CGparameter p, int x, int y, int z, int w)
{ (void)p; (void)x; (void)y; (void)z; (void)w; }
CG_API void CGENTRY cgSetParameter1iv(CGparameter p, const int *v)
{ (void)p; (void)v; }
CG_API void CGENTRY cgSetParameter2iv(CGparameter p, const int *v)
{ (void)p; (void)v; }
CG_API void CGENTRY cgSetParameter3iv(CGparameter p, const int *v)
{ (void)p; (void)v; }
CG_API void CGENTRY cgSetParameter4iv(CGparameter p, const int *v)
{ (void)p; (void)v; }

/* matrix parameter setters/getters: int variants only.
 * float/double variants are macro-aliased to cgGL* forms. */
CG_API void CGENTRY cgSetMatrixParameterir(CGparameter p, const int *m)
{ (void)p; (void)m; }
CG_API void CGENTRY cgSetMatrixParameteric(CGparameter p, const int *m)
{ (void)p; (void)m; }
CG_API void CGENTRY cgGetMatrixParameterir(CGparameter p, int *m)
{ (void)p; if (m) cg_zero(m, 16 * sizeof(int)); }
CG_API void CGENTRY cgGetMatrixParameteric(CGparameter p, int *m)
{ (void)p; if (m) cg_zero(m, 16 * sizeof(int)); }
CG_API void CGENTRY cgGetMatrixParameterdr(CGparameter p, double *m)
{ (void)p; if (m) cg_zero(m, 16 * sizeof(double)); }
CG_API void CGENTRY cgGetMatrixParameterfr(CGparameter p, float *m)
{ (void)p; if (m) cg_zero(m, 16 * sizeof(float)); }
CG_API void CGENTRY cgGetMatrixParameterdc(CGparameter p, double *m)
{ (void)p; if (m) cg_zero(m, 16 * sizeof(double)); }
CG_API void CGENTRY cgGetMatrixParameterfc(CGparameter p, float *m)
{ (void)p; if (m) cg_zero(m, 16 * sizeof(float)); }

CG_API CGparameter CGENTRY cgGetNamedSubParameter(CGparameter param,
                                                  const char *name)
{ (void)param; (void)name; return NULL; }

/* ── type system ─────────────────────────────────────────────────── */

CG_API const char *CGENTRY cgGetTypeString(CGtype type)
{ (void)type; return NULL; }
CG_API CGtype CGENTRY cgGetType(const char *type_string)
{ (void)type_string; return (CGtype)0; }
CG_API CGtype CGENTRY cgGetNamedUserType(CGhandle handle, const char *name)
{ (void)handle; (void)name; return (CGtype)0; }
CG_API int CGENTRY cgGetNumUserTypes(CGhandle handle)
{ (void)handle; return 0; }
CG_API CGtype CGENTRY cgGetUserType(CGhandle handle, int index)
{ (void)handle; (void)index; return (CGtype)0; }
CG_API int CGENTRY cgGetNumParentTypes(CGtype type)
{ (void)type; return 0; }
CG_API CGtype CGENTRY cgGetParentType(CGtype type, int index)
{ (void)type; (void)index; return (CGtype)0; }
CG_API CGbool CGENTRY cgIsParentType(CGtype parent, CGtype child)
{ (void)parent; (void)child; return CG_FALSE; }
CG_API CGbool CGENTRY cgIsInterfaceType(CGtype type)
{ (void)type; return CG_FALSE; }

CG_API const char *CGENTRY cgGetResourceString(CGresource resource)
{ (void)resource; return NULL; }
CG_API CGresource CGENTRY cgGetResource(const char *resource_string)
{ (void)resource_string; return (CGresource)0; }
CG_API const char *CGENTRY cgGetEnumString(CGenum en)
{ (void)en; return NULL; }
CG_API CGenum CGENTRY cgGetEnum(const char *enum_string)
{ (void)enum_string; return 0; }
CG_API const char *CGENTRY cgGetProfileString(CGprofile profile)
{ (void)profile; return NULL; }
CG_API CGprofile CGENTRY cgGetProfile(const char *profile_string)
{ (void)profile_string; return (CGprofile)0; }

/* ── combined programs ───────────────────────────────────────────── */

CG_API int CGENTRY cgGetNumProgramDomains(CGprogram program)
{ (void)program; return 0; }
CG_API CGdomain CGENTRY cgGetProfileDomain(CGprofile profile)
{
    if (psgl_cg_profile_is_vertex(profile)) return CG_VERTEX_DOMAIN;
    if (psgl_cg_profile_is_fragment(profile)) return CG_FRAGMENT_DOMAIN;
    return CG_UNKNOWN_DOMAIN;
}
CG_API CGprogram CGENTRY cgCombinePrograms(int n, const CGprogram *exeList)
{ (void)n; (void)exeList; return NULL; }
CG_API CGprogram CGENTRY cgCombinePrograms2(const CGprogram e1,
                                            const CGprogram e2)
{ (void)e1; (void)e2; return NULL; }
CG_API CGprogram CGENTRY cgCombinePrograms3(const CGprogram e1,
                                            const CGprogram e2,
                                            const CGprogram e3)
{ (void)e1; (void)e2; (void)e3; return NULL; }
CG_API CGprofile CGENTRY cgGetProgramDomainProfile(CGprogram program,
                                                   int index)
{ (void)program; (void)index; return (CGprofile)0; }

/* ── obj (compilation unit) ──────────────────────────────────────── */

CG_API CGobj CGENTRY cgCreateObj(CGcontext ctx, CGenum program_type,
                                 const char *source, CGprofile profile,
                                 const char **args)
{ (void)ctx; (void)program_type; (void)source;
  (void)profile; (void)args; return NULL; }
CG_API CGobj CGENTRY cgCreateObjFromFile(CGcontext ctx, CGenum program_type,
                                         const char *source_file,
                                         CGprofile profile,
                                         const char **args)
{ (void)ctx; (void)program_type; (void)source_file;
  (void)profile; (void)args; return NULL; }
CG_API void CGENTRY cgDestroyObj(CGobj obj) { (void)obj; }

/* ── buffer ──────────────────────────────────────────────────────── */

CG_API long CGENTRY cgGetParameterResourceSize(CGparameter p)
{ (void)p; return 0L; }
CG_API int CGENTRY cgGetParameterBufferIndex(CGparameter p)
{ (void)p; return -1; }
CG_API int CGENTRY cgGetParameterBufferOffset(CGparameter p)
{ (void)p; return 0; }
CG_API CGbuffer CGENTRY cgCreateBuffer(CGcontext ctx, unsigned long size)
{ (void)ctx; (void)size; return NULL; }
CG_API void CGENTRY cgSetBufferData(CGbuffer b, unsigned long size,
                                    const void *data)
{ (void)b; (void)size; (void)data; }
CG_API void CGENTRY cgSetBufferSubData(CGbuffer b, unsigned long offset,
                                       unsigned long size, const void *data)
{ (void)b; (void)offset; (void)size; (void)data; }
CG_API void CGENTRY cgSetProgramBuffer(CGprogram prog, int idx, CGbuffer b)
{ (void)prog; (void)idx; (void)b; }
CG_API void *CGENTRY cgMapBuffer(CGbuffer b, CGbufferaccess access)
{ (void)b; (void)access; return NULL; }
CG_API void CGENTRY cgUnmapBuffer(CGbuffer b) { (void)b; }
CG_API void CGENTRY cgDestroyBuffer(CGbuffer b) { (void)b; }
CG_API CGbuffer CGENTRY cgGetProgramBuffer(CGprogram p, int idx)
{ (void)p; (void)idx; return NULL; }
CG_API unsigned long CGENTRY cgGetBufferSize(CGbuffer b)
{ (void)b; return 0UL; }

/* ── type class helpers ──────────────────────────────────────────── */

CG_API CGparameterclass CGENTRY cgGetTypeClass(CGtype type)
{ return cg_class_for_type(type); }
CG_API CGtype CGENTRY cgGetTypeBase(CGtype type)
{ (void)type; return (CGtype)0; }
CG_API CGbool CGENTRY cgGetTypeSizes(CGtype type, int *nrows, int *ncols)
{
    int rows = cg_rows_for_type(type);
    int cols = cg_columns_for_type(type);
    if (nrows) *nrows = rows;
    if (ncols) *ncols = cols;
    return (rows && cols) ? CG_TRUE : CG_FALSE;
}
CG_API void CGENTRY cgGetMatrixSize(CGtype type, int *nrows, int *ncols)
{
    if (nrows) *nrows = cg_rows_for_type(type);
    if (ncols) *ncols = cg_columns_for_type(type);
}

/* ── PS3-specific Cg runtime init ────────────────────────────────── */

CG_API void CGENTRY psglFXInit(void)  {}
CG_API void CGENTRY psglFXExit(void)  {}
