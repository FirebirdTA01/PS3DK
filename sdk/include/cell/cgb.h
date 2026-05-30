/*! \file cell/cgb.h
 \brief cellCgb - Cg binary loader / introspection (stub surface).

  Minimal header to let reference SDK code that includes
  `<cell/cgb.h>` compile.  Full cellCgb* runtime isn't implemented
  yet; this declares the types the reference SDK's gcmutil.h and
  similar sample-common headers reference in their struct layouts,
  so source-compat works even when the sample doesn't actually call
  any cellCgb API.

  When an actual consumer surfaces, flesh out the function decls +
  add the SPRX stub to build-cell-stub-archives.sh.
*/

#ifndef __PS3DK_CELL_CGB_H__
#define __PS3DK_CELL_CGB_H__

#include <cell/cgb/cgb_struct.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_CGB_OK             0
#define CELL_CGB_ERROR_FAILED   0xffffffff

typedef enum CellCgbProfile {
    CELL_CGB_PROFILE_UNKNOWN = 0,
    CELL_CGB_PROFILE_VERTEX  = 1,
    CELL_CGB_PROFILE_FRAGMENT = 2
} CellCgbProfile;

/* Opaque program handle.  Reference SDK ships an internal-layout
 * char-array body sized by CELL_CGB_PROGRAM_STRUCTURE_SIZE; we expose
 * the same shape so by-value declarations (`CellCgbProgram p;`) work.
 * Body layout is reserved to libcgb; samples only ever pass pointers
 * into the cellCgb* APIs. */
#ifndef CELL_CGB_PROGRAM_STRUCTURE_SIZE
#define CELL_CGB_PROGRAM_STRUCTURE_SIZE  256u
#endif
typedef struct CellCgbProgram {
    char data[CELL_CGB_PROGRAM_STRUCTURE_SIZE];
} CellCgbProgram;

typedef struct CellCgbParameter CellCgbParameter;

/* Reference SDK profile names — kept distinct from the enum so source
 * code that switches on either flavor compiles. */
#define CellCgbVertexProfile     CELL_CGB_PROFILE_VERTEX
#define CellCgbFragmentProfile   CELL_CGB_PROFILE_FRAGMENT

/* Loader / introspection — lightweight wrappers that walk the
 * binary container in-place.  Implementation lives in libgcm_cmd.a
 * alongside the cellGcmCg* family (same blob layout). */
uint32_t       cellCgbGetSize(const void *binary);
int32_t        cellCgbRead(const void *binary, const uint32_t size,
                           CellCgbProgram *program);
CellCgbProfile cellCgbGetProfile(const CellCgbProgram *program);
const void    *cellCgbGetUCode(const CellCgbProgram *program);
uint32_t       cellCgbGetUCodeSize(const CellCgbProgram *program);
int32_t        cellCgbGetVertexConfiguration(const CellCgbProgram *program,
                                             CellCgbVertexProgramConfiguration *conf);
int32_t        cellCgbGetFragmentConfiguration(const CellCgbProgram *program,
                                               CellCgbFragmentProgramConfiguration *conf);
int32_t        cellCgbGetVertexAttributeOutputMask(const CellCgbProgram *program,
                                                    uint32_t *attributeOutputMask);
int32_t        cellCgbGetUserClipPlaneControlMask(const CellCgbProgram *program,
                                                   uint32_t *userClipPlaneControlMask);
uint32_t       cellCgbGetVertexConstantCount(const CellCgbProgram *program);
void           cellCgbGetVertexConstantValues(const CellCgbProgram *program,
                                              uint32_t value_index,
                                              uint16_t *reg,
                                              const float **value);
uint32_t       cellCgbMapLookup(CellCgbProgram *program, const char *name);
uint16_t       cellCgbMapGetValue(CellCgbProgram *program,
                                   const uint32_t map_index);
uint32_t       cellCgbMapGetLength(const CellCgbProgram *program);
void           cellCgbMapGetName(CellCgbProgram *program,
                                  const uint32_t map_index,
                                  char *name, uint32_t *size);
void           cellCgbMapGetVertexUniformRegister(const CellCgbProgram *program,
                                                   const uint32_t map_index,
                                                   uint16_t *reg,
                                                   const float **default_values);
void           cellCgbMapGetFragmentUniformOffsets(const CellCgbProgram *program,
                                                    const uint32_t map_index,
                                                    uint16_t *offsets,
                                                    uint32_t *count);
void           cellCgbMapGetFragmentUniformRegister(const CellCgbProgram *program,
                                                     const uint32_t map_index,
                                                     uint16_t *reg);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CGB_H__ */
