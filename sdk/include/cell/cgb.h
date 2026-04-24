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

/* Opaque forward-declaration structs.  Layout is internal to cellCgb.
 * sizeof() is not queried by the current sample set; if that changes
 * we'll fill in fields per reference SDK 475.001 cell/cgb/cgb_struct.h. */
typedef struct CellCgbProgram                        CellCgbProgram;
typedef struct CellCgbVertexProgramConfiguration     CellCgbVertexProgramConfiguration;
typedef struct CellCgbFragmentProgramConfiguration   CellCgbFragmentProgramConfiguration;
typedef struct CellCgbParameter                      CellCgbParameter;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_CGB_H__ */
