#ifndef PS3DK_LIBPSGL_CG_INTERNAL_H
#define PS3DK_LIBPSGL_CG_INTERNAL_H

#include <Cg/cgBinary.h>
#include <Cg/cgGL.h>
#include <cell/cgb.h>
#include <stdint.h>

#define PSGL_CG_CONTEXT_MAGIC   0x43474354u
#define PSGL_CG_PROGRAM_MAGIC   0x43475052u
#define PSGL_CG_PARAMETER_MAGIC 0x43475041u
#define PSGL_CG_NAME_MAX        64u

typedef struct _CGcontext PSGLcgContext;
typedef struct _CGprogram PSGLcgProgram;
typedef struct _CGparameter PSGLcgParameter;

struct _CGparameter {
    uint32_t magic;
    PSGLcgProgram *program;
    uint32_t index;
    char name[PSGL_CG_NAME_MAX];
    CGtype type;
    CGresource resource;
    CGenum variability;
    CGenum direction;
    int32_t resource_index;
    uint16_t vertex_register;
    uint16_t fragment_register;
    float value[16];
    uint32_t value_count;
    CGbool dirty;
    CGbool state_matrix;
    CGGLenum state_matrix_source;
    CGGLenum state_matrix_transform;
};

struct _CGprogram {
    uint32_t magic;
    PSGLcgContext *context;
    PSGLcgProgram *next;
    PSGLcgProgram *prev;
    CGprofile profile;
    CellCgbProfile cgb_profile;
    void *binary;
    uint32_t binary_size;
    CellCgbProgram cgb_program;
    PSGLcgParameter *parameters;
    uint32_t parameter_count;
    void *fragment_ucode_address;
    uint32_t fragment_ucode_offset;
    CGbool loaded;
};

struct _CGcontext {
    uint32_t magic;
    PSGLcgProgram *programs;
    CGerror last_error;
    CGbool vertex_profile_enabled;
    CGbool fragment_profile_enabled;
};

PSGLcgContext *psgl_cg_context(CGcontext ctx);
PSGLcgProgram *psgl_cg_program(CGprogram program);
PSGLcgParameter *psgl_cg_parameter(CGparameter parameter);
void psgl_cg_set_error(PSGLcgContext *context, CGerror error);
int psgl_cg_profile_is_vertex(CGprofile profile);
int psgl_cg_profile_is_fragment(CGprofile profile);
void psgl_cg_set_parameter_floats(PSGLcgParameter *parameter,
                                  const float *values, uint32_t count);
void psgl_cg_get_parameter_floats(PSGLcgParameter *parameter,
                                  float *values, uint32_t count);
void psgl_cg_set_parameter_matrix(PSGLcgParameter *parameter,
                                  const float *values);
void psgl_cg_get_parameter_matrix(PSGLcgParameter *parameter,
                                  float *values);

#endif
