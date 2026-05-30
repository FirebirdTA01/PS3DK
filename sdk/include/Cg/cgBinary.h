#ifndef PS3DK_CG_BINARY_H
#define PS3DK_CG_BINARY_H

#include <stdint.h>
#include <Cg/cg.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CG_BINARY_FORMAT_REVISION 0x00000006u

typedef uint32_t CgBinaryOffset;
typedef CgBinaryOffset CgBinaryStringOffset;
typedef CgBinaryOffset CgBinaryFloatOffset;
typedef CgBinaryOffset CgBinaryParameterOffset;
typedef CgBinaryOffset CgBinaryEmbeddedConstantOffset;

typedef struct CgBinaryParameter {
    CGtype type;
    CGresource res;
    CGenum var;
    int32_t resIndex;
    CgBinaryStringOffset name;
    CgBinaryFloatOffset defaultValue;
    CgBinaryEmbeddedConstantOffset embeddedConst;
    CgBinaryStringOffset semantic;
    CGenum direction;
    int32_t paramno;
    CGbool isReferenced;
    CGbool isShared;
} CgBinaryParameter;

typedef struct CgBinaryEmbeddedConstant {
    uint32_t ucodeCount;
    uint32_t ucodeOffset[1];
} CgBinaryEmbeddedConstant;

typedef struct CgBinaryVertexProgram {
    uint32_t instructionCount;
    uint32_t instructionSlot;
    uint32_t registerCount;
    uint32_t attributeInputMask;
    uint32_t attributeOutputMask;
    uint32_t userClipMask;
} CgBinaryVertexProgram;

typedef struct CgBinaryFragmentProgram {
    uint32_t instructionCount;
    uint32_t attributeInputMask;
    uint32_t partialTexType;
    uint16_t texCoordsInputMask;
    uint16_t texCoords2D;
    uint16_t texCoordsCentroid;
    uint8_t registerCount;
    uint8_t outputFromH0;
    uint8_t depthReplace;
    uint8_t pixelKill;
} CgBinaryFragmentProgram;

typedef struct CgBinaryProgram {
    CGprofile profile;
    uint32_t revision;
    uint32_t totalSize;
    uint32_t parameterCount;
    CgBinaryParameterOffset parameterArray;
    CgBinaryOffset program;
    uint32_t ucodeSize;
    CgBinaryOffset ucode;
    uint8_t data[1];
} CgBinaryProgram;

#ifdef __cplusplus
}
#endif

#endif /* PS3DK_CG_BINARY_H */
