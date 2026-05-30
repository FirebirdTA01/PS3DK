/* cgc_compiler.c — sceCgc runtime-compiler stubs (Slice 1d link gate).
 */
#include <Cg/cgc.h>
#include <string.h>

CGCcontext *sceCgcNewContext(CGCmem *pool)
{ (void)pool; return NULL; }

void sceCgcDeleteContext(CGCcontext *context) { (void)context; }

CGCbin *sceCgcNewBin(CGCmem *pool)
{ (void)pool; return NULL; }

void *sceCgcGetBinData(CGCbin *bin)
{ (void)bin; return NULL; }

size_t sceCgcGetBinSize(CGCbin *bin)
{ (void)bin; return 0; }

CGCstatus sceCgcStoreBinData(CGCbin *bin, void *data, size_t size)
{ (void)bin; (void)data; (void)size; return SCECGC_OK; }

void sceCgcDeleteBin(CGCbin *bin) { (void)bin; }

CGCstatus sceCgcCompileString(CGCcontext *context,
                              const char *sourceString,
                              const char *profile,
                              const char *entry,
                              const char **options,
                              CGCbin *shaderBinary,
                              CGCbin *messages,
                              CGCbin *asciiOutput,
                              CGCinclude *includeHandler)
{
    (void)context;     (void)sourceString;
    (void)profile;     (void)entry;
    (void)options;     (void)shaderBinary;
    (void)messages;    (void)asciiOutput;
    (void)includeHandler;
    return SCECGC_ERROR_NO_PROGRAM;
}

#ifndef __CELLOS_LV2__
CGCstatus sceCgcCompileFile(CGCcontext *context,
                            const char *sourceFile,
                            const char *profile,
                            const char *entry,
                            const char **options,
                            CGCbin *shaderBinary,
                            CGCbin *messages,
                            CGCbin *asciiOutput,
                            CGCinclude *includeHandler)
{
    (void)context;     (void)sourceFile;
    (void)profile;     (void)entry;
    (void)options;     (void)shaderBinary;
    (void)messages;    (void)asciiOutput;
    (void)includeHandler;
    return SCECGC_ERROR_CANT_OPEN_FILE;
}
#endif

int compile_program_from_string(const char *prog,
                                const char *profile_string,
                                const char *entry,
                                const char **args,
                                char **bout)
{ (void)prog; (void)profile_string; (void)entry; (void)args;
  if (bout) *bout = NULL;
  return -1; }

void free_compiled_program(char *bout) { (void)bout; }
