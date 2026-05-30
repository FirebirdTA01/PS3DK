/* report.c — PSGL report/debug stubs (Slice 1d link gate).
 */
#include <PSGL/report.h>
#include <stddef.h>

PSGL_EXPORT void psglDisableReport(GLenum report)
{ (void)report; }

PSGL_EXPORT void psglEnableReport(GLenum report)
{ (void)report; }

PSGL_EXPORT GLboolean psglIsReportEnabled(GLenum report)
{ (void)report; return GL_FALSE; }

PSGL_EXPORT void psglSetReportClassMask(GLuint reportClassMask)
{ (void)reportClassMask; }

PSGL_EXPORT GLuint psglGetReportClassMask(void)
{ return 0; }

PSGL_EXPORT PSGLreportFunction psglGetReportFunction(void)
{ return NULL; }

PSGL_EXPORT void psglSetReportFunction(PSGLreportFunction function)
{ (void)function; }

PSGL_EXPORT void psglSetDefaultReportParameters(GLboolean showEnums)
{ (void)showEnums; }
