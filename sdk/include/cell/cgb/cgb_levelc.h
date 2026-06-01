#ifndef PS3DK_CELL_CGB_LEVELC_H
#define PS3DK_CELL_CGB_LEVELC_H

#include <cell/cgb.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Level C compatibility surface over the CgBinaryProgram container that
 * cellCgbRead already validates.  This derives Level-C-style parameter
 * semantics from CgBinaryParameter; emitting a literal Level A/B/C block
 * remains a separate format-compatibility refinement.
 */
uint16_t cellCgbLevelCMapGetCgType(CellCgbProgram *program,
                                   const uint32_t map_index);
uint16_t cellCgbLevelCMapGetCgResource(CellCgbProgram *program,
                                       const uint32_t map_index);
uint16_t cellCgbLevelCMapGetVariability(CellCgbProgram *program,
                                        const uint32_t map_index);
uint16_t cellCgbLevelCMapGetDirection(CellCgbProgram *program,
                                      const uint32_t map_index);
uint16_t cellCgbLevelCMapGetValueCount(CellCgbProgram *program,
                                       const uint32_t map_index);

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_CELL_CGB_LEVELC_H */
