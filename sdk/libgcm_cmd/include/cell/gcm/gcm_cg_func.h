/*
 * PS3 Custom Toolchain — cellGcmCg* struct-walker API.
 *
 * Declarations for the subset of the cellGcmCg* family that inspects
 * an already-loaded CgBinaryProgram blob.  Our implementation lives
 * in libgcm_cmd.a; the returned values are pulled directly from the
 * binary format defined in <Cg/cgBinary.h>.
 *
 * We ship a practical subset — enough to bring up basic graphics
 * samples.  The remaining ~20 cellGcmCg* entry points (Set*, Cgb*,
 * embedded-constant iteration) can be added incrementally; the
 * struct fields they would read are already exposed by cgBinary.h.
 */

#ifndef PS3TC_CELL_GCM_CG_FUNC_H
#define PS3TC_CELL_GCM_CG_FUNC_H

#include <stdint.h>
#include <Cg/cgBinary.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time fix-up of a freshly-loaded CgBinaryProgram blob.  On PPU
 * the payload is already big-endian so the conversion is a no-op; we
 * keep the entry point for source compatibility with cell-SDK-style
 * samples that call it unconditionally. */
void cellGcmCgInitProgram(CGprogram prog);

/* Returns pointer to the raw ucode blob and its byte size.  Either
 * out-parameter may be NULL. */
void cellGcmCgGetUCode(CGprogram prog, void **ucode, uint32_t *ucode_size);

/* Total size (in bytes) of the CgBinaryProgram blob, including the
 * trailing string / parameter / program-header data. */
uint32_t cellGcmCgGetTotalBinarySize(CGprogram prog);

/* Profile tag from the container header (sce_vp_rsx / sce_fp_rsx / …). */
uint32_t cellGcmCgGetProgramProfile(CGprogram prog);

/* Vertex-program attributes — only valid when the profile is vertex. */
uint32_t cellGcmCgGetRegisterCount(CGprogram prog);
uint32_t cellGcmCgGetInstructionSlot(CGprogram prog);
uint32_t cellGcmCgGetInstructions(CGprogram prog);
uint32_t cellGcmCgGetVertexAttribInputMask(CGprogram prog);
uint32_t cellGcmCgGetAttribOutputMask(CGprogram prog);
uint32_t cellGcmCgGetVertexUserClipMask(CGprogram prog);

/* Mutators — patch the FP / VP container header in place after load. */
void cellGcmCgSetRegisterCount(CGprogram prog, uint32_t count);
void cellGcmCgSetAttribOutputMask(CGprogram prog, uint32_t mask);

/* Parameter table iteration. */
uint32_t    cellGcmCgGetCountParameter(CGprogram prog);
CGparameter cellGcmCgGetIndexParameter(CGprogram prog, uint32_t index);
CGparameter cellGcmCgGetNamedParameter(CGprogram prog, const char *name);

/* Treat the parameter table as a linear sequence of leaves.  `Next`
 * returns NULL once the end is reached. */
CGparameter cellGcmCgGetFirstLeafParameter(CGprogram prog);
CGparameter cellGcmCgGetNextLeafParameter(CGprogram prog, CGparameter param);

/* Per-parameter accessors. */
uint32_t    cellGcmCgGetParameterType         (CGprogram prog, CGparameter param);
uint32_t    cellGcmCgGetParameterResource     (CGprogram prog, CGparameter param);
int32_t     cellGcmCgGetParameterResourceIndex(CGprogram prog, CGparameter param);
uint32_t    cellGcmCgGetParameterVariability  (CGprogram prog, CGparameter param);
uint32_t    cellGcmCgGetParameterDirection    (CGprogram prog, CGparameter param);
int32_t     cellGcmCgGetParameterOrdinalNumber(CGprogram prog, CGparameter param);
CGbool      cellGcmCgGetParameterReferenced   (CGprogram prog, CGparameter param);
const char *cellGcmCgGetParameterName         (CGprogram prog, CGparameter param);
const char *cellGcmCgGetParameterSemantic     (CGprogram prog, CGparameter param);
const float *cellGcmCgGetParameterValues      (CGprogram prog, CGparameter param);

/* Embedded-constant iteration — fragment shaders only; the returned
 * offsets are into the ucode blob returned by cellGcmCgGetUCode. */
uint32_t       cellGcmCgGetEmbeddedConstantCount (CGprogram prog, CGparameter param);
uint32_t       cellGcmCgGetEmbeddedConstantOffset(CGprogram prog, CGparameter param, uint32_t i);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_GCM_CG_FUNC_H */
