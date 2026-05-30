/*
 * PS3 Custom Toolchain -- <cell/dbgrsx/dbgrsx_bundle_state_struct.h>
 *
 * Struct types for each graphics bundle state slot supported by
 * cellDbgRsxGetGraphicsBundleState (libdbgrsx.a).  Each struct
 * mirrors the bit-level layout of the corresponding RSX hardware
 * register or method payload so that the debugger can report
 * decoded state without needing knowledge of the RSX command format.
 */

#ifndef __PS3DK_CELL_DBGRSX_BUNDLE_STATE_STRUCT_H__
#define __PS3DK_CELL_DBGRSX_BUNDLE_STATE_STRUCT_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- POLYGON_STIPPLE --- */
typedef struct CellDbgRsxBundleStatePolygonStipple {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePolygonStipple;

/* --- LINE_STIPPLE --- */
typedef struct CellDbgRsxBundleStateLineStipple {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateLineStipple;

/* --- LINE_STIPPLE_PATTERN --- */
typedef struct CellDbgRsxBundleStateLineStipplePattern {
	unsigned pattern : 16;
	unsigned factor  : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateLineStipplePattern;

/* --- WINDOW_OFFSET --- */
typedef struct CellDbgRsxBundleStateWindowOffset {
	unsigned y : 16;
	unsigned x : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateWindowOffset;

/* --- SHADER_PROGRAM --- */
typedef struct CellDbgRsxBundleStateShaderProgram {
	unsigned offset   : 26;
	unsigned padding  : 4;
	unsigned location : 2;
} __attribute__((aligned(4))) CellDbgRsxBundleStateShaderProgram;

/* --- SURFACE_COLOR_TARGET --- */
typedef struct CellDbgRsxBundleStateSurfaceColorTarget {
	unsigned padding     : 27;
	unsigned colorTarget : 5;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceColorTarget;

/* --- FOG_PARAMS --- */
typedef struct CellDbgRsxBundleStateFogParams {
	struct {
		unsigned value;
	} data[2];
} __attribute__((aligned(4))) CellDbgRsxBundleStateFogParams;

/* --- POINT_SIZE --- */
typedef struct CellDbgRsxBundleStatePointSize {
	uint32_t data0;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePointSize;

/* --- SEMAPHORE_OFFSET --- */
typedef struct CellDbgRsxBundleStateSemaphoreOffset {
	unsigned padding0 : 8;
	unsigned index    : 20;
	unsigned padding1 : 4;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSemaphoreOffset;

/* --- SURFACE_CLIP_HORIZONTAL --- */
typedef struct CellDbgRsxBundleStateSurfaceClipHorizontal {
	unsigned width : 16;
	unsigned x     : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceClipHorizontal;

/* --- SURFACE_CLIP_VERTICAL --- */
typedef struct CellDbgRsxBundleStateSurfaceClipVertical {
	unsigned height : 16;
	unsigned y      : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceClipVertical;

/* --- ANTI_ALIASING_CONTROL --- */
typedef struct CellDbgRsxBundleStateAntiAliasingControl {
	unsigned sampleMask      : 16;
	unsigned padding         : 4;
	unsigned alphaToOne      : 4;
	unsigned alphaToCoverage : 4;
	unsigned enable          : 4;
} __attribute__((aligned(4))) CellDbgRsxBundleStateAntiAliasingControl;

/* --- BLEND_EQUATION --- */
typedef struct CellDbgRsxBundleStateBlendEquation {
	unsigned alpha : 16;
	unsigned color : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBlendEquation;

/* --- BLEND_FUNC_SFACTOR --- */
typedef struct CellDbgRsxBundleStateBlendFuncSfactor {
	unsigned sfAlpha : 16;
	unsigned sfColor : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBlendFuncSfactor;

/* --- BLEND_FUNC_DFACTOR --- */
typedef struct CellDbgRsxBundleStateBlendFuncDfactor {
	unsigned dfAlpha : 16;
	unsigned dfColor : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBlendFuncDfactor;

/* --- BLEND_ENABLE --- */
typedef struct CellDbgRsxBundleStateBlendEnable {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBlendEnable;

/* --- BLEND_ENABLE_MRT --- */
typedef struct CellDbgRsxBundleStateBlendEnableMrt {
	unsigned padding0 : 28;
	unsigned mrt3     : 1;
	unsigned mrt2     : 1;
	unsigned mrt1     : 1;
	unsigned padding1 : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBlendEnableMrt;

/* --- LOGIC_OP --- */
typedef struct CellDbgRsxBundleStateLogicOp {
	unsigned op : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateLogicOp;

/* --- LOGIC_OP_ENABLE --- */
typedef struct CellDbgRsxBundleStateLogicOpEnable {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateLogicOpEnable;

/* --- BLEND_COLOR --- */
typedef struct CellDbgRsxBundleStateBlendColor {
	unsigned color : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBlendColor;

/* --- BLEND_COLOR2 --- */
typedef struct CellDbgRsxBundleStateBlendColor2 {
	unsigned color : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBlendColor2;

/* --- COLOR_CLEAR_VALUE --- */
typedef struct CellDbgRsxBundleStateColorClearValue {
	unsigned color : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateColorClearValue;

/* --- BACK_STENCIL_FUNC --- */
typedef struct CellDbgRsxBundleStateBackStencilFunc {
	unsigned func : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackStencilFunc;

/* --- BACK_STENCIL_FUNC_REF --- */
typedef struct CellDbgRsxBundleStateBackStencilFuncRef {
	unsigned ref : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackStencilFuncRef;

/* --- BACK_STENCIL_FUNC_MASK --- */
typedef struct CellDbgRsxBundleStateBackStencilFuncMask {
	unsigned mask : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackStencilFuncMask;

/* --- BACK_STENCIL_MASK --- */
typedef struct CellDbgRsxBundleStateBackStencilMask {
	unsigned sm : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackStencilMask;

/* --- BACK_STENCIL_OP_FAIL --- */
typedef struct CellDbgRsxBundleStateBackStencilOpFail {
	unsigned fail : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackStencilOpFail;

/* --- BACK_STENCIL_OP_ZFAIL --- */
typedef struct CellDbgRsxBundleStateBackStencilOpZfail {
	unsigned depthFail : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackStencilOpZfail;

/* --- BACK_STENCIL_OP_ZPASS --- */
typedef struct CellDbgRsxBundleStateBackStencilOpZpass {
	unsigned depthPass : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackStencilOpZpass;

/* --- USER_CLIP_PLANE_CONTROL --- */
typedef struct CellDbgRsxBundleStateUserClipPlaneControl {
	unsigned padding : 8;
	unsigned plane5  : 4;
	unsigned plane4  : 4;
	unsigned plane3  : 4;
	unsigned plane2  : 4;
	unsigned plane1  : 4;
	unsigned plane0  : 4;
} __attribute__((aligned(4))) CellDbgRsxBundleStateUserClipPlaneControl;

/* --- VIEWPORT_HORIZONTAL --- */
typedef struct CellDbgRsxBundleStateViewportHorizontal {
	unsigned width : 16;
	unsigned x     : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateViewportHorizontal;

/* --- VIEWPORT_VERTICAL --- */
typedef struct CellDbgRsxBundleStateViewportVertical {
	unsigned height : 16;
	unsigned y      : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateViewportVertical;

/* --- SCISSOR_HORIZONTAL --- */
typedef struct CellDbgRsxBundleStateScissorHorizontal {
	unsigned width : 16;
	unsigned x     : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateScissorHorizontal;

/* --- SCISSOR_VERTICAL --- */
typedef struct CellDbgRsxBundleStateScissorVertical {
	unsigned height : 16;
	unsigned y      : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateScissorVertical;

/* --- ZSTENCIL_CLEAR_VALUE --- */
typedef struct CellDbgRsxBundleStateZstencilClearValue {
	unsigned value : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateZstencilClearValue;

/* --- CLIP_MIN --- */
typedef struct CellDbgRsxBundleStateClipMin {
	unsigned min : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateClipMin;

/* --- CLIP_MAX --- */
typedef struct CellDbgRsxBundleStateClipMax {
	unsigned max : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateClipMax;

/* --- POLYGON_OFFSET_SCALE_FACTOR --- */
typedef struct CellDbgRsxBundleStatePolygonOffsetScaleFactor {
	unsigned factor : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePolygonOffsetScaleFactor;

/* --- POLYGON_OFFSET_BIAS --- */
typedef struct CellDbgRsxBundleStatePolygonOffsetBias {
	unsigned units : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePolygonOffsetBias;

/* --- RESTART_INDEX_ENABLE --- */
typedef struct CellDbgRsxBundleStateRestartIndexEnable {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateRestartIndexEnable;

/* --- RESTART_INDEX --- */
typedef struct CellDbgRsxBundleStateRestartIndex {
	unsigned index : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateRestartIndex;

/* --- CYLINDRICAL_WRAP --- */
typedef struct CellDbgRsxBundleStateCylindricalWrap {
	unsigned data : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateCylindricalWrap;

/* --- SHADER_CONTROL --- */
typedef struct CellDbgRsxBundleStateShaderControl {
	unsigned registerCount : 8;
	unsigned padding0      : 8;
	unsigned controlTxp    : 1;
	unsigned padding1      : 4;
	unsigned always_1      : 1;
	unsigned padding2      : 2;
	unsigned pixelKill     : 1;
	unsigned outputFromH0  : 1;
	unsigned padding3      : 2;
	unsigned depthReplace  : 3;
	unsigned padding4      : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateShaderControl;

/* --- REDUCE_DST_COLOR --- */
typedef struct CellDbgRsxBundleStateReduceDstColor {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateReduceDstColor;

/* --- SHADER_WINDOW --- */
typedef struct CellDbgRsxBundleStateShaderWindow {
	unsigned padding     : 12;
	unsigned pixelCenter : 4;
	unsigned origin      : 4;
	unsigned height      : 12;
} __attribute__((aligned(4))) CellDbgRsxBundleStateShaderWindow;

/* --- DEPTH_BOUNDS_MIN --- */
typedef struct CellDbgRsxBundleStateDepthBoundsMin {
	unsigned zmin : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateDepthBoundsMin;

/* --- DEPTH_BOUNDS_MAX --- */
typedef struct CellDbgRsxBundleStateDepthBoundsMax {
	unsigned zmax : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateDepthBoundsMax;

/* --- VERTEX_ATTRIB_INPUT_MASK --- */
typedef struct CellDbgRsxBundleStateVertexAttribInputMask {
	unsigned padding : 16;
	unsigned ATTR15  : 1;
	unsigned ATTR14  : 1;
	unsigned ATTR13  : 1;
	unsigned ATTR12  : 1;
	unsigned ATTR11  : 1;
	unsigned ATTR10  : 1;
	unsigned ATTR09  : 1;
	unsigned ATTR08  : 1;
	unsigned ATTR07  : 1;
	unsigned ATTR06  : 1;
	unsigned ATTR05  : 1;
	unsigned ATTR04  : 1;
	unsigned ATTR03  : 1;
	unsigned ATTR02  : 1;
	unsigned ATTR01  : 1;
	unsigned ATTR00  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexAttribInputMask;

/* --- VERTEX_ATTRIB_OUTPUT_MASK --- */
typedef struct CellDbgRsxBundleStateVertexAttribOutputMask {
	unsigned padding   : 10;
	unsigned TEXCOORD9 : 1;
	unsigned TEXCOORD8 : 1;
	unsigned TEXCOORD7 : 1;
	unsigned TEXCOORD6 : 1;
	unsigned TEXCOORD5 : 1;
	unsigned TEXCOORD4 : 1;
	unsigned TEXCOORD3 : 1;
	unsigned TEXCOORD2 : 1;
	unsigned TEXCOORD1 : 1;
	unsigned TEXCOORD0 : 1;
	unsigned CLP5      : 1;
	unsigned CLP4      : 1;
	unsigned CLP3      : 1;
	unsigned CLP2      : 1;
	unsigned CLP1      : 1;
	unsigned CLP0      : 1;
	unsigned PSIZE     : 1;
	unsigned FOG       : 1;
	unsigned BCOL1     : 1;
	unsigned BCOL0     : 1;
	unsigned COLOR1    : 1;
	unsigned COLOR0    : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexAttribOutputMask;

/* --- TEX_COORD_CONTROL --- */
typedef struct CellDbgRsxBundleStateTexCoordControl {
	struct {
		unsigned padding           : 24;
		unsigned texCoordsCentroid : 4;
		unsigned texCoords2D       : 4;
	} data[10];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTexCoordControl;

/* --- VERTEX_DATA_BASE_OFFSET --- */
typedef struct CellDbgRsxBundleStateVertexDataBaseOffset {
	unsigned baseOffset : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexDataBaseOffset;

/* --- VERTEX_DATA_BASE_INDEX --- */
typedef struct CellDbgRsxBundleStateVertexDataBaseIndex {
	unsigned baseIndex : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexDataBaseIndex;

/* --- TRANSFORM_TIMEOUT --- */
typedef struct CellDbgRsxBundleStateTransformTimeout {
	unsigned padding       : 8;
	unsigned registerCount : 8;
	unsigned count         : 16;
} __attribute__((aligned(4))) CellDbgRsxBundleStateTransformTimeout;

/* --- SHADER_PACKER --- */
typedef struct CellDbgRsxBundleStateShaderPacker {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateShaderPacker;

/* --- TRANSFORM_BRANCH_BITS --- */
typedef struct CellDbgRsxBundleStateTransformBranchBits {
	unsigned branchBits : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateTransformBranchBits;

/* --- COLOR_MASK --- */
typedef struct CellDbgRsxBundleStateColorMask {
	unsigned AlphaWriteEnable : 8;
	unsigned RedWriteEnable   : 8;
	unsigned GreenWriteEnable : 8;
	unsigned BlueWriteEnable  : 8;
} __attribute__((aligned(4))) CellDbgRsxBundleStateColorMask;

/* --- TRANSFORM_PROGRAM_LOAD --- */
typedef struct CellDbgRsxBundleStateTransformProgramLoad {
	unsigned padding  : 8;
	unsigned loadSlot : 24;
} __attribute__((aligned(4))) CellDbgRsxBundleStateTransformProgramLoad;

/* --- TRANSFORM_CONSTANT_LOAD --- */
typedef struct CellDbgRsxBundleStateTransformConstantLoad {
	unsigned padding  : 8;
	unsigned loadSlot : 24;
} __attribute__((aligned(4))) CellDbgRsxBundleStateTransformConstantLoad;

/* --- TEXTURE_OFFSET --- */
typedef struct CellDbgRsxBundleStateTextureOffset {
	struct { unsigned offset : 32; } data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureOffset;

/* --- TEXTURE_FORMAT --- */
typedef struct CellDbgRsxBundleStateTextureFormat {
	struct {
		unsigned padding    : 12;
		unsigned mipmap     : 4;
		unsigned format     : 8;
		unsigned dimension  : 4;
		unsigned bordermode : 1;
		unsigned cubemap    : 1;
		unsigned location   : 2;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureFormat;

/* --- TEXTURE_ADDRESS --- */
typedef struct CellDbgRsxBundleStateTextureAddress {
	struct {
		unsigned zfunc         : 4;
		unsigned padding       : 4;
		unsigned gamma_a       : 1;
		unsigned gamma_b       : 1;
		unsigned gamma_g       : 1;
		unsigned gamma_r       : 1;
		unsigned wrapr         : 4;
		unsigned unsignedRemap : 4;
		unsigned wrapt         : 4;
		unsigned anisoBias     : 4;
		unsigned wraps         : 4;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureAddress;

/* --- TEXTURE_CONTROL0 --- */
typedef struct CellDbgRsxBundleStateTextureControl0 {
	struct {
		unsigned enable    : 1;
		unsigned minLod    : 12;
		unsigned maxLod    : 12;
		unsigned maxAniso  : 3;
		unsigned padding0  : 1;
		unsigned AlphaKill : 1;
		unsigned padding1  : 2;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureControl0;

/* --- TEXTURE_CONTROL1 --- */
typedef struct CellDbgRsxBundleStateTextureControl1 {
	struct {
		unsigned padding    : 12;
		unsigned RemapOrder : 4;
		unsigned OutB       : 2;
		unsigned OutG       : 2;
		unsigned OutR       : 2;
		unsigned OutA       : 2;
		unsigned InB        : 2;
		unsigned InG        : 2;
		unsigned InR        : 2;
		unsigned InA        : 2;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureControl1;

/* --- TEXTURE_CONTROL2 --- */
typedef struct CellDbgRsxBundleStateTextureControl2 {
	struct {
		unsigned padding0 : 24;
		unsigned aniso    : 1;
		unsigned iso      : 1;
		unsigned padding1 : 1;
		unsigned slope    : 5;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureControl2;

/* --- TEXTURE_CONTROL3 --- */
typedef struct CellDbgRsxBundleStateTextureControl3 {
	struct {
		unsigned padding0 : 2;
		unsigned depth    : 10;
		unsigned padding1 : 2;
		unsigned pitch    : 18;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureControl3;

/* --- TEXTURE_FILTER --- */
typedef struct CellDbgRsxBundleStateTextureFilter {
	struct {
		unsigned BSigned : 1;
		unsigned GSigned : 1;
		unsigned RSigned : 1;
		unsigned ASigned : 1;
		unsigned mag     : 4;
		unsigned min     : 8;
		unsigned conv    : 3;
		unsigned bias    : 13;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureFilter;

/* --- TEXTURE_IMAGE_RECT --- */
typedef struct CellDbgRsxBundleStateTextureImageRect {
	struct {
		unsigned width  : 16;
		unsigned height : 16;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureImageRect;

/* --- TEXTURE_BORDER_COLOR --- */
typedef struct CellDbgRsxBundleStateTextureBorderColor {
	struct { unsigned color : 32; } data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateTextureBorderColor;

/* --- VERTEX_TEXTURE_OFFSET --- */
typedef struct CellDbgRsxBundleStateVertexTextureOffset {
	struct { unsigned offset : 32; } data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureOffset;

/* --- VERTEX_TEXTURE_FORMAT --- */
typedef struct CellDbgRsxBundleStateVertexTextureFormat {
	struct {
		unsigned padding0  : 12;
		unsigned mipmap    : 4;
		unsigned format    : 8;
		unsigned dimension : 4;
		unsigned padding1  : 2;
		unsigned location  : 2;
	} data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureFormat;

/* --- VERTEX_TEXTURE_ADDRESS --- */
typedef struct CellDbgRsxBundleStateVertexTextureAddress {
	struct {
		unsigned padding0 : 20;
		unsigned wrapt    : 4;
		unsigned padding1 : 4;
		unsigned wraps    : 4;
	} data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureAddress;

/* --- VERTEX_TEXTURE_CONTROL0 --- */
typedef struct CellDbgRsxBundleStateVertexTextureControl0 {
	struct {
		unsigned enable  : 1;
		unsigned minLod  : 12;
		unsigned maxLod  : 12;
		unsigned padding : 7;
	} data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureControl0;

/* --- VERTEX_TEXTURE_CONTROL3 --- */
typedef struct CellDbgRsxBundleStateVertexTextureControl3 {
	struct {
		unsigned padding : 14;
		unsigned pitch   : 18;
	} data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureControl3;

/* --- VERTEX_TEXTURE_FILTER --- */
typedef struct CellDbgRsxBundleStateVertexTextureFilter {
	struct {
		unsigned padding : 19;
		unsigned bias    : 13;
	} data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureFilter;

/* --- VERTEX_TEXTURE_IMAGE_RECT --- */
typedef struct CellDbgRsxBundleStateVertexTextureImageRect {
	struct {
		unsigned width  : 16;
		unsigned height : 16;
	} data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureImageRect;

/* --- VERTEX_TEXTURE_BORDER_COLOR --- */
typedef struct CellDbgRsxBundleStateVertexTextureBorderColor {
	struct { unsigned color : 32; } data[4];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexTextureBorderColor;

/* --- ANISO_SPREAD --- */
typedef struct CellDbgRsxBundleStateAnisoSpread {
	struct {
		unsigned padding0             : 8;
		unsigned vReduceSamplesEnable : 4;
		unsigned vSpacingSelect       : 4;
		unsigned hReduceSamplesEnable : 4;
		unsigned hSpacingSelect       : 4;
		unsigned reduceSamplesEnable  : 4;
		unsigned spacingSelect        : 4;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateAnisoSpread;

/* --- ALPHA_FUNC --- */
typedef struct CellDbgRsxBundleStateAlphaFunc {
	unsigned af : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateAlphaFunc;

/* --- ALPHA_REF --- */
typedef struct CellDbgRsxBundleStateAlphaRef {
	unsigned ref : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateAlphaRef;

/* --- ALPHA_TEST_ENABLE --- */
typedef struct CellDbgRsxBundleStateAlphaTestEnable {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateAlphaTestEnable;

/* --- DEPTH_TEST_ENABLE --- */
typedef struct CellDbgRsxBundleStateDepthTestEnable {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateDepthTestEnable;

/* --- DEPTH_FUNC --- */
typedef struct CellDbgRsxBundleStateDepthFunc {
	unsigned zf : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateDepthFunc;

/* --- DITHER_ENABLE --- */
typedef struct CellDbgRsxBundleStateDitherEnable {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateDitherEnable;

/* --- DEPTH_MASK --- */
typedef struct CellDbgRsxBundleStateDepthMask {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateDepthMask;

/* --- STENCIL_TEST_ENABLE --- */
typedef struct CellDbgRsxBundleStateStencilTestEnable {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilTestEnable;

/* --- TWO_SIDED_STENCIL_TEST_ENABLE --- */
typedef struct CellDbgRsxBundleStateTwoSidedStencilTestEnable {
	unsigned enable : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateTwoSidedStencilTestEnable;

/* --- STENCIL_FUNC --- */
typedef struct CellDbgRsxBundleStateStencilFunc {
	unsigned func : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilFunc;

/* --- STENCIL_FUNC_REF --- */
typedef struct CellDbgRsxBundleStateStencilFuncRef {
	unsigned ref : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilFuncRef;

/* --- STENCIL_FUNC_MASK --- */
typedef struct CellDbgRsxBundleStateStencilFuncMask {
	unsigned mask : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilFuncMask;

/* --- STENCIL_MASK --- */
typedef struct CellDbgRsxBundleStateStencilMask {
	unsigned sm : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilMask;

/* --- STENCIL_OP_FAIL --- */
typedef struct CellDbgRsxBundleStateStencilOpFail {
	unsigned fail : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilOpFail;

/* --- STENCIL_OP_ZFAIL --- */
typedef struct CellDbgRsxBundleStateStencilOpZfail {
	unsigned depthFail : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilOpZfail;

/* --- STENCIL_OP_ZPASS --- */
typedef struct CellDbgRsxBundleStateStencilOpZpass {
	unsigned depthPass : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateStencilOpZpass;

/* --- SHADE_MODE --- */
typedef struct CellDbgRsxBundleStateShadeMode {
	unsigned sm : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateShadeMode;

/* --- POINT_SPRITE_CONTROL --- */
typedef struct CellDbgRsxBundleStatePointSpriteControl {
	unsigned padding0  : 14;
	unsigned texcoord9 : 1;
	unsigned texcoord8 : 1;
	unsigned texcoord7 : 1;
	unsigned texcoord6 : 1;
	unsigned texcoord5 : 1;
	unsigned texcoord4 : 1;
	unsigned texcoord3 : 1;
	unsigned texcoord2 : 1;
	unsigned texcoord1 : 1;
	unsigned texcoord0 : 1;
	unsigned padding1  : 5;
	unsigned rmode     : 2;
	unsigned enable    : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePointSpriteControl;

/* --- FOG_MODE --- */
typedef struct CellDbgRsxBundleStateFogMode {
	unsigned mode : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateFogMode;

/* --- TRANSFORM_PROGRAM_START --- */
typedef struct CellDbgRsxBundleStateTransformProgramStart {
	unsigned startSlot : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateTransformProgramStart;

/* --- SURFACE_FORMAT --- */
typedef struct CellDbgRsxBundleStateSurfaceFormat {
	unsigned height      : 8;
	unsigned width       : 8;
	unsigned antialias   : 4;
	unsigned type        : 4;
	unsigned depthFormat : 3;
	unsigned colorFormat : 5;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceFormat;

/* --- SURFACE_COLOR_AOFFSET --- */
typedef struct CellDbgRsxBundleStateSurfaceColorAoffset {
	unsigned colorOffset : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceColorAoffset;

/* --- SURFACE_COLOR_BOFFSET --- */
typedef struct CellDbgRsxBundleStateSurfaceColorBoffset {
	unsigned colorOffset : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceColorBoffset;

/* --- SURFACE_COLOR_COFFSET --- */
typedef struct CellDbgRsxBundleStateSurfaceColorCoffset {
	unsigned colorOffset : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceColorCoffset;

/* --- SURFACE_COLOR_DOFFSET --- */
typedef struct CellDbgRsxBundleStateSurfaceColorDoffset {
	unsigned colorOffset : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceColorDoffset;

/* --- SURFACE_PITCH_A --- */
typedef struct CellDbgRsxBundleStateSurfacePitchA {
	unsigned padding    : 15;
	unsigned colorPitch : 17;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfacePitchA;

/* --- SURFACE_PITCH_B --- */
typedef struct CellDbgRsxBundleStateSurfacePitchB {
	unsigned padding    : 15;
	unsigned colorPitch : 17;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfacePitchB;

/* --- SURFACE_PITCH_C --- */
typedef struct CellDbgRsxBundleStateSurfacePitchC {
	unsigned padding    : 15;
	unsigned colorPitch : 17;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfacePitchC;

/* --- SURFACE_PITCH_D --- */
typedef struct CellDbgRsxBundleStateSurfacePitchD {
	unsigned padding    : 15;
	unsigned colorPitch : 17;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfacePitchD;

/* --- SURFACE_ZETA_OFFSET --- */
typedef struct CellDbgRsxBundleStateSurfaceZetaOffset {
	unsigned depthOffset : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfaceZetaOffset;

/* --- SURFACE_PITCH_Z --- */
typedef struct CellDbgRsxBundleStateSurfacePitchZ {
	unsigned padding    : 15;
	unsigned depthPitch : 17;
} __attribute__((aligned(4))) CellDbgRsxBundleStateSurfacePitchZ;

/* --- COLOR_MASK_MRT --- */
typedef struct CellDbgRsxBundleStateColorMaskMrt {
	unsigned padding0               : 16;
	unsigned BlueWriteEnableB_MRT3  : 1;
	unsigned GreenWriteEnableB_MRT3 : 1;
	unsigned RedWriteEnableB_MRT3   : 1;
	unsigned AlphaWriteEnableB_MRT3 : 1;
	unsigned BlueWriteEnableB_MRT2  : 1;
	unsigned GreenWriteEnableB_MRT2 : 1;
	unsigned RedWriteEnableB_MRT2   : 1;
	unsigned AlphaWriteEnableB_MRT2 : 1;
	unsigned BlueWriteEnableB_MRT1  : 1;
	unsigned GreenWriteEnableB_MRT1 : 1;
	unsigned RedWriteEnableB_MRT1   : 1;
	unsigned AlphaWriteEnableB_MRT1 : 1;
	unsigned padding1               : 4;
} __attribute__((aligned(4))) CellDbgRsxBundleStateColorMaskMrt;

/* --- VERTEX_DATA_ARRAY_FORMAT --- */
typedef struct CellDbgRsxBundleStateVertexDataArrayFormat {
	struct {
		unsigned padding : 16;
		unsigned Stride  : 8;
		unsigned Size    : 4;
		unsigned Type    : 4;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexDataArrayFormat;

/* --- VERTEX_DATA_ARRAY_OFFSET --- */
typedef struct CellDbgRsxBundleStateVertexDataArrayOffset {
	struct {
		unsigned location : 1;
		unsigned offset   : 31;
	} data[16];
} __attribute__((aligned(4))) CellDbgRsxBundleStateVertexDataArrayOffset;

/* --- CONTEXT_DMA_COLOR_A --- */
typedef struct CellDbgRsxBundleStateContextDmaColorA {
	unsigned padding  : 31;
	unsigned location : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateContextDmaColorA;

/* --- CONTEXT_DMA_COLOR_B --- */
typedef struct CellDbgRsxBundleStateContextDmaColorB {
	unsigned padding  : 31;
	unsigned location : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateContextDmaColorB;

/* --- CONTEXT_DMA_COLOR_C --- */
typedef struct CellDbgRsxBundleStateContextDmaColorC {
	unsigned padding  : 31;
	unsigned location : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateContextDmaColorC;

/* --- CONTEXT_DMA_COLOR_D --- */
typedef struct CellDbgRsxBundleStateContextDmaColorD {
	unsigned padding  : 31;
	unsigned location : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateContextDmaColorD;

/* --- CONTEXT_DMA_ZETA --- */
typedef struct CellDbgRsxBundleStateContextDmaZeta {
	unsigned padding  : 31;
	unsigned location : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateContextDmaZeta;

/* --- FRONT_FACE --- */
typedef struct CellDbgRsxBundleStateFrontFace {
	unsigned dir : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateFrontFace;

/* --- CULL_FACE --- */
typedef struct CellDbgRsxBundleStateCullFace {
	unsigned cfm : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateCullFace;

/* --- CULL_FACE_ENABLE --- */
typedef struct CellDbgRsxBundleStateCullFaceEnable {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateCullFaceEnable;

/* --- LINE_WIDTH --- */
typedef struct CellDbgRsxBundleStateLineWidth {
	unsigned padding : 23;
	unsigned width   : 9;
} __attribute__((aligned(4))) CellDbgRsxBundleStateLineWidth;

/* --- LINE_SMOOTH_ENABLE --- */
typedef struct CellDbgRsxBundleStateLineSmoothEnable {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateLineSmoothEnable;

/* --- POLY_OFFSET_FILL_ENABLE --- */
typedef struct CellDbgRsxBundleStatePolyOffsetFillEnable {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePolyOffsetFillEnable;

/* --- POLY_OFFSET_LINE_ENABLE --- */
typedef struct CellDbgRsxBundleStatePolyOffsetLineEnable {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePolyOffsetLineEnable;

/* --- POLY_SMOOTH_ENABLE --- */
typedef struct CellDbgRsxBundleStatePolySmoothEnable {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStatePolySmoothEnable;

/* --- FRONT_POLYGON_MODE --- */
typedef struct CellDbgRsxBundleStateFrontPolygonMode {
	unsigned mode : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateFrontPolygonMode;

/* --- BACK_POLYGON_MODE --- */
typedef struct CellDbgRsxBundleStateBackPolygonMode {
	unsigned mode : 32;
} __attribute__((aligned(4))) CellDbgRsxBundleStateBackPolygonMode;

/* --- TWO_SIDE_LIGHT_EN --- */
typedef struct CellDbgRsxBundleStateTwoSideLightEn {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateTwoSideLightEn;

/* --- CONTROL0 --- */
typedef struct CellDbgRsxBundleStateControl0 {
	unsigned padding0 : 19;
	unsigned format   : 1;
	unsigned padding1 : 12;
} __attribute__((aligned(4))) CellDbgRsxBundleStateControl0;

/* --- DEPTH_BOUNDS_TEST_ENABLE --- */
typedef struct CellDbgRsxBundleStateDepthBoundsTestEnable {
	unsigned padding : 31;
	unsigned enable  : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateDepthBoundsTestEnable;

/* --- FREQUENCY_DIVIDER_OPERATION --- */
typedef struct CellDbgRsxBundleStateFrequencyDividerOperation {
	unsigned padding  : 16;
	unsigned ATTR15   : 1;
	unsigned ATTR14   : 1;
	unsigned ATTR13   : 1;
	unsigned ATTR12   : 1;
	unsigned ATTR11   : 1;
	unsigned ATTR10   : 1;
	unsigned ATTR09   : 1;
	unsigned ATTR08   : 1;
	unsigned ATTR07   : 1;
	unsigned ATTR06   : 1;
	unsigned ATTR05   : 1;
	unsigned ATTR04   : 1;
	unsigned ATTR03   : 1;
	unsigned ATTR02   : 1;
	unsigned ATTR01   : 1;
	unsigned ATTR00   : 1;
} __attribute__((aligned(4))) CellDbgRsxBundleStateFrequencyDividerOperation;

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_DBGRSX_BUNDLE_STATE_STRUCT_H__ */
