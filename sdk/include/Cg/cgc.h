#ifndef PS3DK_CG_CGC_H
#define PS3DK_CG_CGC_H

#include <stddef.h>

#ifndef DLLEXPORT
#define DLLEXPORT
#endif

#ifdef __cplusplus
#define SCECGC_DEFAULT_0 = 0
#else
#define SCECGC_DEFAULT_0
#endif

enum {
    SCECGC_ERROR_NO_IHANDLER = -6,
    SCECGC_ERROR_NO_PROGRAM = -5,
    SCECGC_ERROR_CANT_OPEN_FILE = -4,
    SCECGC_ERROR_BAD_ALLOC = -3,
    SCECGC_ERROR_INVALID_OPTION = -2,
    SCECGC_ERROR_UNEXPECTED = -1,
    SCECGC_OK = 0
};

typedef int CGCstatus;
typedef void CGCbin;
typedef void CGCcontext;

typedef struct CGCmem {
    void *(*malloc)(void *arg, size_t size);
    void (*free)(void *arg, void *ptr);
    void *arg;
} CGCmem;

typedef enum SCECGC_INCLUDE_TYPE {
    SCECGC_LOCAL_INCLUDE,
    SCECGC_SYSTEM_INCLUDE
} SCECGC_INCLUDE_TYPE;

typedef struct CGCinclude {
    int (*open)(SCECGC_INCLUDE_TYPE type, const char *filename, char **data, size_t *size);
    int (*close)(const char *data);
} CGCinclude;

#ifdef __cplusplus
extern "C" {
#endif

DLLEXPORT CGCcontext *sceCgcNewContext(CGCmem *pool SCECGC_DEFAULT_0);
DLLEXPORT void sceCgcDeleteContext(CGCcontext *context);
DLLEXPORT CGCbin *sceCgcNewBin(CGCmem *pool SCECGC_DEFAULT_0);
DLLEXPORT void *sceCgcGetBinData(CGCbin *bin);
DLLEXPORT size_t sceCgcGetBinSize(CGCbin *bin);
DLLEXPORT CGCstatus sceCgcStoreBinData(CGCbin *bin, void *data, size_t size);
DLLEXPORT void sceCgcDeleteBin(CGCbin *bin);
DLLEXPORT CGCstatus sceCgcCompileString(CGCcontext *context, const char *sourceString, const char *profile, const char *entry, const char **options, CGCbin *shaderBinary, CGCbin *messages SCECGC_DEFAULT_0, CGCbin *asciiOutput SCECGC_DEFAULT_0, CGCinclude *includeHandler SCECGC_DEFAULT_0);
#ifndef __CELLOS_LV2__
DLLEXPORT CGCstatus sceCgcCompileFile(CGCcontext *context, const char *sourceFile, const char *profile, const char *entry, const char **options, CGCbin *shaderBinary, CGCbin *messages SCECGC_DEFAULT_0, CGCbin *asciiOutput SCECGC_DEFAULT_0, CGCinclude *includeHandler SCECGC_DEFAULT_0);
#endif
DLLEXPORT int compile_program_from_string(const char *prog, const char *profile_string, const char *entry, const char **args, char **bout);
DLLEXPORT void free_compiled_program(char *bout);

#ifdef __cplusplus
}
#endif

#undef SCECGC_DEFAULT_0

#endif /* PS3DK_CG_CGC_H */
