/* cgb.c — cellCgb binary-reader stubs (Slice 1d link gate).
 *
 * Returns zero counts, NULL pointers, CELL_CGB_ERROR_FAILED for failures.
 * All output parameters are zeroed on failure paths so callers see a
 * known-zero state, not stack garbage.
 */
#include <cell/cgb.h>
#include <string.h>

uint32_t cellCgbGetSize(const void *binary)
{ (void)binary; return 0; }

int32_t cellCgbRead(const void *binary, const uint32_t size,
                    CellCgbProgram *program)
{ (void)binary; (void)size;
  if (program) memset(program, 0, sizeof(*program));
  return CELL_CGB_ERROR_FAILED; }

CellCgbProfile cellCgbGetProfile(const CellCgbProgram *program)
{ (void)program; return CELL_CGB_PROFILE_UNKNOWN; }

const void *cellCgbGetUCode(const CellCgbProgram *program)
{ (void)program; return NULL; }

uint32_t cellCgbGetUCodeSize(const CellCgbProgram *program)
{ (void)program; return 0; }

int32_t cellCgbGetVertexConfiguration(
    const CellCgbProgram *program,
    CellCgbVertexProgramConfiguration *conf)
{ (void)program;
  if (conf) memset(conf, 0, sizeof(*conf));
  return CELL_CGB_ERROR_FAILED; }

int32_t cellCgbGetFragmentConfiguration(
    const CellCgbProgram *program,
    CellCgbFragmentProgramConfiguration *conf)
{ (void)program;
  if (conf) memset(conf, 0, sizeof(*conf));
  return CELL_CGB_ERROR_FAILED; }

int32_t cellCgbGetVertexAttributeOutputMask(const CellCgbProgram *program,
                                            uint32_t *attributeOutputMask)
{ (void)program;
  if (attributeOutputMask) *attributeOutputMask = 0;
  return CELL_CGB_ERROR_FAILED; }

int32_t cellCgbGetUserClipPlaneControlMask(const CellCgbProgram *program,
                                           uint32_t *userClipPlaneControlMask)
{ (void)program;
  if (userClipPlaneControlMask) *userClipPlaneControlMask = 0;
  return CELL_CGB_ERROR_FAILED; }

uint32_t cellCgbGetVertexConstantCount(const CellCgbProgram *program)
{ (void)program; return 0; }

void cellCgbGetVertexConstantValues(const CellCgbProgram *program,
                                    uint32_t value_index,
                                    uint16_t *reg,
                                    const float **value)
{ (void)program; (void)value_index;
  if (reg)   *reg   = 0;
  if (value) *value = NULL; }

uint32_t cellCgbMapLookup(CellCgbProgram *program, const char *name)
{ (void)program; (void)name; return CELL_CGB_ERROR_FAILED; }

uint16_t cellCgbMapGetValue(CellCgbProgram *program,
                            const uint32_t map_index)
{ (void)program; (void)map_index; return 0; }

uint32_t cellCgbMapGetLength(const CellCgbProgram *program)
{ (void)program; return 0; }

void cellCgbMapGetName(CellCgbProgram *program,
                       const uint32_t map_index,
                       char *name, uint32_t *size)
{ (void)program; (void)map_index;
  if (name) name[0] = '\0';
  if (size) *size = 0; }

void cellCgbMapGetVertexUniformRegister(const CellCgbProgram *program,
                                        const uint32_t map_index,
                                        uint16_t *reg,
                                        const float **default_values)
{ (void)program; (void)map_index;
  if (reg)            *reg            = 0;
  if (default_values) *default_values = NULL; }

void cellCgbMapGetFragmentUniformOffsets(const CellCgbProgram *program,
                                         const uint32_t map_index,
                                         uint16_t *offsets,
                                         uint32_t *count)
{ (void)program; (void)map_index;
  if (offsets) *offsets = 0;
  if (count)   *count   = 0; }

void cellCgbMapGetFragmentUniformRegister(const CellCgbProgram *program,
                                          const uint32_t map_index,
                                          uint16_t *reg)
{ (void)program; (void)map_index;
  if (reg) *reg = 0; }
