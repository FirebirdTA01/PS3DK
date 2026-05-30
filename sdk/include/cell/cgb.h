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

#ifndef CELL_CGB_PROGRAM_STRUCTURE_SIZE
#define CELL_CGB_PROGRAM_STRUCTURE_SIZE  256u
#endif
typedef struct CellCgbProgram {
    char data[CELL_CGB_PROGRAM_STRUCTURE_SIZE];
} CellCgbProgram;

typedef struct CellCgbParameter CellCgbParameter;

#define CellCgbVertexProfile     CELL_CGB_PROFILE_VERTEX
#define CellCgbFragmentProfile   CELL_CGB_PROFILE_FRAGMENT

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
