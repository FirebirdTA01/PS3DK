/*
 * PS3 Custom Toolchain — Cg binary program layout.
 *
 * Describes the on-disk format of the .vpo / .fpo shader blobs that
 * the NVIDIA Cg compiler (sce-cgc) emits and that `cellGcmCg*`
 * runtime helpers walk at load time.  This is a binary-data-format
 * specification; implementations of walkers over it must match the
 * layout bit-for-bit to interoperate with compiled shaders.
 *
 * The format revision we target is 0x06 (PS3 SDK 4.x series).  All
 * `CgBinaryOffset` values are byte offsets measured from the start of
 * the outer CgBinaryProgram blob; they are stored on disk as 32-bit
 * big-endian integers and the runtime is responsible for byte-swap
 * fix-up on little-endian hosts (not applicable on PPU — native BE).
 *
 * Clean-room: authored from the documented wire format, not derived
 * from any vendor source file.
 */

#ifndef PS3TC_CG_CGBINARY_H
#define PS3TC_CG_CGBINARY_H

#include <stdint.h>
#include <Cg/cg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Current binary-format revision number.  CgBinaryProgram.revision must
 * equal this for our loader to treat the blob as valid. */
#define PS3TC_CG_BINARY_REVISION  0x00000006u

/* Opaque on-disk offset.  Always measured from the start of the blob. */
typedef uint32_t CgBinaryOffset;

/* Parameter metadata record.  One per named parameter exposed by the
 * shader (vertex inputs / fragment varyings / uniforms / samplers).
 *
 * `name` and `semantic` are offsets to NUL-terminated C strings.
 * `defaultValue` points to a float array for uniforms that were
 * initialised in source; may be zero for "no default".
 * `embeddedConst` points to a CgBinaryEmbeddedConstant giving the
 * list of ucode word offsets that need to be patched when the host
 * writes a new value into this parameter (fragment shaders only —
 * vertex shaders load constants at program upload).
 *
 * `type`, `res`, `var`, `direction` hold enum values from the Cg
 * runtime; callers may treat them as opaque 32-bit tags or reproduce
 * the NVIDIA Cg symbolic names independently.
 */
struct CgBinaryParameter
{
    CGtype          type;
    CGresource      res;
    CGenum          var;
    int32_t         resIndex;
    CgBinaryOffset  name;
    CgBinaryOffset  defaultValue;
    CgBinaryOffset  embeddedConst;
    CgBinaryOffset  semantic;
    CGenum          direction;
    int32_t         paramno;
    CGbool          isReferenced;
    CGbool          isShared;
};

/* Fragment-program constant patch list.  The ucodeOffset array is
 * `ucodeCount` words long and lives inline at the end of the record. */
struct CgBinaryEmbeddedConstant
{
    uint32_t ucodeCount;
    uint32_t ucodeOffset[1];
};

/* Vertex-program header placed at `CgBinaryProgram.program` for shaders
 * whose profile is a vertex variant. */
struct CgBinaryVertexProgram
{
    uint32_t instructionCount;
    uint32_t instructionSlot;
    uint32_t registerCount;
    uint32_t attributeInputMask;
    uint32_t attributeOutputMask;
    uint32_t userClipMask;
};

/* Fragment-program header placed at `CgBinaryProgram.program` for
 * shaders whose profile is a fragment variant. */
struct CgBinaryFragmentProgram
{
    uint32_t instructionCount;
    uint32_t attributeInputMask;
    uint32_t partialTexType;
    uint16_t texCoordsInputMask;
    uint16_t texCoords2D;
    uint16_t texCoordsCentroid;
    uint8_t  registerCount;
    uint8_t  outputFromH0;
    uint8_t  depthReplace;
    uint8_t  pixelKill;
};

/* The outer container.  Parameters, program-specific header, ucode,
 * and string table all live at offsets measured from `this`. */
struct CgBinaryProgram
{
    CGprofile       profile;
    uint32_t        revision;
    uint32_t        totalSize;
    uint32_t        parameterCount;
    CgBinaryOffset  parameterArray;
    CgBinaryOffset  program;
    uint32_t        ucodeSize;
    CgBinaryOffset  ucode;
    uint8_t         data[1];
};

/* Compat typedefs so client code can use the vendor names. */
typedef struct CgBinaryParameter         CgBinaryParameter;
typedef struct CgBinaryEmbeddedConstant  CgBinaryEmbeddedConstant;
typedef struct CgBinaryVertexProgram     CgBinaryVertexProgram;
typedef struct CgBinaryFragmentProgram   CgBinaryFragmentProgram;
typedef struct CgBinaryProgram           CgBinaryProgram;

/* CGprogram / CGparameter are defined in <Cg/cg.h>. */

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CG_CGBINARY_H */
