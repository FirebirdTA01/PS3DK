/* cg_runtime.c — Cg runtime API stubs (Slice 1d link gate).
 *
 * Covers cg.h / NV/cg.h surface.  Pointer-returning functions return
 * NULL + set CG_INVALID_PARAMETER_ERROR where appropriate; is-functions
 * return CG_FALSE; void functions are NOPs.
 */
#include <Cg/cg.h>
#include <string.h>

static CGerror g_cg_error = CG_NO_ERROR;

/* ── error ───────────────────────────────────────────────────────── */

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
    g_cg_error = CG_MEMORY_ALLOC_ERROR;
    return NULL;
}

CG_API void CGENTRY cgDestroyContext(CGcontext ctx) { (void)ctx; }

CG_API CGbool CGENTRY cgIsContext(CGcontext ctx)
{ (void)ctx; return CG_FALSE; }

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
{ (void)ctx; (void)program_type; (void)program_file;
  (void)profile; (void)entry; (void)args; return NULL; }

CG_API CGprogram CGENTRY cgCopyProgram(CGprogram program)
{
    (void)program;
    g_cg_error = CG_INVALID_PARAMETER_HANDLE_ERROR;
    return NULL;
}

CG_API void CGENTRY cgDestroyProgram(CGprogram program)
{ (void)program; }

CG_API CGprogram CGENTRY cgGetFirstProgram(CGcontext ctx)
{ (void)ctx; return NULL; }

CG_API CGprogram CGENTRY cgGetNextProgram(CGprogram current)
{ (void)current; return NULL; }

CG_API CGcontext CGENTRY cgGetProgramContext(CGprogram prog)
{ (void)prog; return NULL; }

CG_API CGbool CGENTRY cgIsProgram(CGprogram program)
{ (void)program; return CG_FALSE; }

CG_API void CGENTRY cgCompileProgram(CGprogram program) { (void)program; }

CG_API CGbool CGENTRY cgIsProgramCompiled(CGprogram program)
{ (void)program; return CG_FALSE; }

CG_API const char *CGENTRY cgGetProgramString(CGprogram prog, CGenum pname)
{ (void)prog; (void)pname; return NULL; }

CG_API CGprofile CGENTRY cgGetProgramProfile(CGprogram prog)
{ (void)prog; return (CGprofile)0; }

CG_API char const *const *CGENTRY cgGetProgramOptions(CGprogram prog)
{ (void)prog; return NULL; }

CG_API void CGENTRY cgSetProgramProfile(CGprogram prog, CGprofile profile)
{ (void)prog; (void)profile; }

CG_API CGenum CGENTRY cgGetProgramInput(CGprogram program)
{ (void)program; return 0; }

CG_API CGenum CGENTRY cgGetProgramOutput(CGprogram program)
{ (void)program; return 0; }

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
{ (void)prog; (void)name; return NULL; }

CG_API CGparameter CGENTRY cgGetNamedProgramParameter(CGprogram prog,
    CGenum name_space, const char *name)
{ (void)prog; (void)name_space; (void)name; return NULL; }

CG_API CGparameter CGENTRY cgGetFirstParameter(CGprogram prog,
                                               CGenum name_space)
{ (void)prog; (void)name_space; return NULL; }

CG_API CGparameter CGENTRY cgGetNextParameter(CGparameter current)
{ (void)current; return NULL; }

CG_API CGparameter CGENTRY cgGetFirstLeafParameter(CGprogram prog,
                                                   CGenum name_space)
{ (void)prog; (void)name_space; return NULL; }

CG_API CGparameter CGENTRY cgGetNextLeafParameter(CGparameter current)
{ (void)current; return NULL; }

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
{ (void)param; return NULL; }
CG_API CGcontext CGENTRY cgGetParameterContext(CGparameter param)
{ (void)param; return NULL; }
CG_API CGbool CGENTRY cgIsParameter(CGparameter param)
{ (void)param; return CG_FALSE; }
CG_API const char *CGENTRY cgGetParameterName(CGparameter param)
{ (void)param; return NULL; }
CG_API CGtype CGENTRY cgGetParameterType(CGparameter param)
{ (void)param; return (CGtype)0; }
CG_API CGtype CGENTRY cgGetParameterBaseType(CGparameter param)
{ (void)param; return (CGtype)0; }
CG_API CGparameterclass CGENTRY cgGetParameterClass(CGparameter param)
{ (void)param; return (CGparameterclass)0; }
CG_API int CGENTRY cgGetParameterRows(CGparameter param)
{ (void)param; return 0; }
CG_API int CGENTRY cgGetParameterColumns(CGparameter param)
{ (void)param; return 0; }
CG_API CGtype CGENTRY cgGetParameterNamedType(CGparameter param)
{ (void)param; return (CGtype)0; }
CG_API const char *CGENTRY cgGetParameterSemantic(CGparameter param)
{ (void)param; return NULL; }
CG_API CGresource CGENTRY cgGetParameterResource(CGparameter param)
{ (void)param; return (CGresource)0; }
CG_API CGresource CGENTRY cgGetParameterBaseResource(CGparameter param)
{ (void)param; return (CGresource)0; }
CG_API unsigned long CGENTRY cgGetParameterResourceIndex(CGparameter param)
{ (void)param; return 0UL; }
CG_API CGenum CGENTRY cgGetParameterVariability(CGparameter param)
{ (void)param; return 0; }
CG_API CGenum CGENTRY cgGetParameterDirection(CGparameter param)
{ (void)param; return 0; }
CG_API CGbool CGENTRY cgIsParameterReferenced(CGparameter param)
{ (void)param; return CG_FALSE; }
CG_API CGbool CGENTRY cgIsParameterUsed(CGparameter param, CGhandle handle)
{ (void)param; (void)handle; return CG_FALSE; }

CG_API const double *CGENTRY cgGetParameterValues(CGparameter param,
    CGenum value_type, int *nvalues)
{ (void)param; (void)value_type;
  if (nvalues) *nvalues = 0;
  return NULL; }

/* parameter set/get — all NOPs */
CG_API void CGENTRY cgSetParameterValuedr(CGparameter p, int n, const double *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValuedc(CGparameter p, int n, const double *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValuefr(CGparameter p, int n, const float *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValuefc(CGparameter p, int n, const float *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValueir(CGparameter p, int n, const int *v)
{ (void)p; (void)n; (void)v; }
CG_API void CGENTRY cgSetParameterValueic(CGparameter p, int n, const int *v)
{ (void)p; (void)n; (void)v; }
CG_API int CGENTRY cgGetParameterValuedr(CGparameter p, int n, double *v)
{ (void)p; (void)n; if (v) memset(v, 0, (size_t)n * sizeof(double)); return 0; }
CG_API int CGENTRY cgGetParameterValuedc(CGparameter p, int n, double *v)
{ (void)p; (void)n; if (v) memset(v, 0, (size_t)n * sizeof(double)); return 0; }
CG_API int CGENTRY cgGetParameterValuefr(CGparameter p, int n, float *v)
{ (void)p; (void)n; if (v) memset(v, 0, (size_t)n * sizeof(float)); return 0; }
CG_API int CGENTRY cgGetParameterValuefc(CGparameter p, int n, float *v)
{ (void)p; (void)n; if (v) memset(v, 0, (size_t)n * sizeof(float)); return 0; }
CG_API int CGENTRY cgGetParameterValueir(CGparameter p, int n, int *v)
{ (void)p; (void)n; if (v) memset(v, 0, (size_t)n * sizeof(int)); return 0; }
CG_API int CGENTRY cgGetParameterValueic(CGparameter p, int n, int *v)
{ (void)p; (void)n; if (v) memset(v, 0, (size_t)n * sizeof(int)); return 0; }

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
{ (void)param; return -1; }
CG_API void CGENTRY cgSetParameterVariability(CGparameter param, CGenum vary)
{ (void)param; (void)vary; }
CG_API void CGENTRY cgSetParameterSemantic(CGparameter param,
                                           const char *semantic)
{ (void)param; (void)semantic; }

/* scalar parameter setters — int variants only.
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

/* matrix parameter setters/getters — int variants only.
 * float/double variants are macro-aliased to cgGL* forms. */
CG_API void CGENTRY cgSetMatrixParameterir(CGparameter p, const int *m)
{ (void)p; (void)m; }
CG_API void CGENTRY cgSetMatrixParameteric(CGparameter p, const int *m)
{ (void)p; (void)m; }
CG_API void CGENTRY cgGetMatrixParameterir(CGparameter p, int *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(int)); }
CG_API void CGENTRY cgGetMatrixParameteric(CGparameter p, int *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(int)); }
CG_API void CGENTRY cgGetMatrixParameterdr(CGparameter p, double *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(double)); }
CG_API void CGENTRY cgGetMatrixParameterfr(CGparameter p, float *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(float)); }
CG_API void CGENTRY cgGetMatrixParameterdc(CGparameter p, double *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(double)); }
CG_API void CGENTRY cgGetMatrixParameterfc(CGparameter p, float *m)
{ (void)p; if (m) memset(m, 0, 16 * sizeof(float)); }

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
{ (void)profile; return (CGdomain)0; }
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
{ (void)type; return (CGparameterclass)0; }
CG_API CGtype CGENTRY cgGetTypeBase(CGtype type)
{ (void)type; return (CGtype)0; }
CG_API CGbool CGENTRY cgGetTypeSizes(CGtype type, int *nrows, int *ncols)
{ (void)type; if (nrows) *nrows = 0; if (ncols) *ncols = 0; return CG_FALSE; }
CG_API void CGENTRY cgGetMatrixSize(CGtype type, int *nrows, int *ncols)
{ (void)type; if (nrows) *nrows = 0; if (ncols) *ncols = 0; }

/* ── PS3-specific Cg runtime init ────────────────────────────────── */

CG_API void CGENTRY psglFXInit(void)  {}
CG_API void CGENTRY psglFXExit(void)  {}
