#ifndef PS3DK_CELL_CGB_STRUCT_H
#define PS3DK_CELL_CGB_STRUCT_H

#include <stdint.h>

#if defined(__SPU__)
#define PS3DK_CGB_PACKED __attribute__((packed))
#else
#define PS3DK_CGB_PACKED
#endif

typedef struct PS3DK_CGB_PACKED CellCgbVertexProgramConfiguration {
    uint16_t instructionSlot;
    uint16_t instructionCount;
    uint16_t attributeInputMask;
    uint8_t registerCount;
    uint8_t unused0;
} CellCgbVertexProgramConfiguration;

typedef struct PS3DK_CGB_PACKED CellCgbFragmentProgramConfiguration {
    uint32_t offset;
    uint32_t attributeInputMask;
    uint16_t texCoordsInputMask;
    uint16_t texCoords2D;
    uint16_t texCoordsCentroid;
    uint16_t unused0;
    uint32_t fragmentControl;
    uint8_t registerCount;
    uint8_t unused1;
    uint16_t unused2;
} CellCgbFragmentProgramConfiguration;

#undef PS3DK_CGB_PACKED

#endif /* PS3DK_CELL_CGB_STRUCT_H */
