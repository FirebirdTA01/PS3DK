#ifndef PS3DK_CG_NV_TYPES_H
#define PS3DK_CG_NV_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int CGbool;
#define CG_FALSE ((CGbool)0)
#define CG_TRUE ((CGbool)1)

typedef struct _CGcontext *CGcontext;
typedef struct _CGprogram *CGprogram;
typedef struct _CGparameter *CGparameter;
typedef struct _CGobj *CGobj;
typedef struct _CGbuffer *CGbuffer;
typedef struct _CGeffect *CGeffect;
typedef struct _CGtechnique *CGtechnique;
typedef struct _CGpass *CGpass;
typedef struct _CGstate *CGstate;
typedef struct _CGstateassignment *CGstateassignment;
typedef struct _CGannotation *CGannotation;
typedef void *CGhandle;

typedef enum CGtype {
    CG_UNKNOWN_TYPE,
    CG_STRUCT,
    CG_ARRAY,
    CG_TYPELESS_STRUCT,
    CG_TYPE_START_ENUM = 1024,
#define CG_DATATYPE_MACRO(name, compiler_name, enum_name, base_name, nrows, ncols, pc_name) enum_name,
#include <Cg/cg_datatypes.h>
#undef CG_DATATYPE_MACRO
} CGtype;

typedef enum CGresource {
#define CG_BINDLOCATION_MACRO(name, enum_name, compiler_name, enum_int, addressable, param_type) enum_name = enum_int,
#include <Cg/cg_bindlocations.h>
#undef CG_BINDLOCATION_MACRO
    CG_UNDEFINED = 3256
} CGresource;

typedef enum CGprofile {
    CG_PROFILE_START = 6144,
    CG_PROFILE_UNKNOWN,
#define CG_PROFILE_MACRO(name, compiler_id, compiler_id_caps, compiler_opt, int_id, vertex_profile) CG_PROFILE_##compiler_id_caps = int_id,
#include <Cg/cg_profiles.h>
#undef CG_PROFILE_MACRO
    CG_PROFILE_MAX = 7100
} CGprofile;

typedef enum CGerror {
#define CG_ERROR_MACRO(code, enum_name, message) enum_name = code,
#include <Cg/cg_errors.h>
#undef CG_ERROR_MACRO
} CGerror;

typedef enum CGenum {
#define CG_ENUM_MACRO(enum_name, enum_val) enum_name = enum_val,
#include <Cg/cg_enums.h>
#undef CG_ENUM_MACRO
} CGenum;

typedef enum CGparameterclass {
    CG_PARAMETERCLASS_UNKNOWN = 0,
    CG_PARAMETERCLASS_SCALAR,
    CG_PARAMETERCLASS_VECTOR,
    CG_PARAMETERCLASS_MATRIX,
    CG_PARAMETERCLASS_STRUCT,
    CG_PARAMETERCLASS_ARRAY,
    CG_PARAMETERCLASS_SAMPLER,
    CG_PARAMETERCLASS_OBJECT
} CGparameterclass;

typedef enum CGdomain {
    CG_UNKNOWN_DOMAIN = 0,
    CG_FIRST_DOMAIN = 1,
    CG_VERTEX_DOMAIN = 1,
    CG_FRAGMENT_DOMAIN,
    CG_GEOMETRY_DOMAIN,
    CG_NUMBER_OF_DOMAINS
} CGdomain;

typedef enum CGbufferaccess {
    CG_MAP_READ = 0,
    CG_MAP_WRITE,
    CG_MAP_READ_WRITE,
    CG_MAP_WRITE_DISCARD,
    CG_MAP_WRITE_NO_OVERWRITE
} CGbufferaccess;

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_CG_NV_TYPES_H */
