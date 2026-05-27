/*
 * PS3 Custom Toolchain - cell/font/libfont.h
 *
 * cellFont library surface: font management, glyph generation,
 * rendering, and layout.  Backed by libcellFont_stub.a.
 */
#ifndef _PS3DK_CELL_FONT_LIBFONT_H_
#define _PS3DK_CELL_FONT_LIBFONT_H_

#include <stdint.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *(*CellFontMallocCallback)(void *, uint32_t);
typedef void  (*CellFontFreeCallback)(void *, void *);
typedef void *(*CellFontReallocCallback)(void *Object, void *p,
                                         uint32_t reallocSize);
typedef void *(*CellFontCallocCallback)(void *, uint32_t, uint32_t);

typedef struct CellFontMemoryInterface {
	void *Object;
	CellFontMallocCallback  Malloc;
	CellFontFreeCallback    Free;
	CellFontReallocCallback Realloc;
	CellFontCallocCallback  Calloc;
} CellFontMemoryInterface;

static inline void CellFontMemoryInterface_initialize(
	CellFontMemoryInterface *mIF)
{
	mIF->Object  = (void *)0;
	mIF->Malloc  = (CellFontMallocCallback )0;
	mIF->Free    = (CellFontFreeCallback   )0;
	mIF->Realloc = (CellFontReallocCallback)0;
	mIF->Calloc  = (CellFontCallocCallback )0;
}

typedef struct CellFontEntry {
	uint32_t lock;
	uint32_t uniqueId;
	const void *fontLib;
	void *fontH;
} CellFontEntry;

typedef struct CellFontConfig {
	struct {
		uint32_t *buffer;
		uint32_t  size;
	} FileCache;
	uint32_t       userFontEntryMax;
	CellFontEntry *userFontEntrys;
	uint32_t       flags;
} CellFontConfig;

static inline void CellFontConfig_initialize(CellFontConfig *config)
{
	config->FileCache.buffer = (uint32_t *)0;
	config->FileCache.size   = 0;
	config->userFontEntryMax = 0;
	config->userFontEntrys   = (CellFontEntry *)0;
	config->flags            = 0x00000000;
}

typedef struct CellFontLibrary {
	uint32_t libraryType, libraryVersion;
	uint32_t SystemClosed[];
} CellFontLibrary;

#define CELL_FONT_LIBRARY_TYPE_NONE (0)

typedef struct CellFontType {
	uint32_t type;
	uint32_t map;
} CellFontType;

typedef struct CellFontHorizontalLayout {
	float baseLineY;
	float lineHeight;
	float effectHeight;
} CellFontHorizontalLayout;

typedef struct CellFontVerticalLayout {
	float baseLineX;
	float lineWidth;
	float effectWidth;
} CellFontVerticalLayout;

typedef struct CellFontGlyphMetrics {
	float width;
	float height;
	struct { float bearingX, bearingY, advance; } Horizontal;
	struct { float bearingX, bearingY, advance; } Vertical;
} CellFontGlyphMetrics;

typedef struct CellFontGlyphOutline {
	int16_t contoursCount;
	int16_t pointsCount;
	struct { float x, y; } *Points;
	uint8_t  *pointTags;
	uint16_t *contourIndexs;
	uint32_t  flags;
	void     *generateEnv;
} CellFontGlyphOutline;

typedef struct CellFontGlyph {
	uint16_t CF_type, type;
	uint32_t size;
	CellFontGlyphMetrics Metrics;
	CellFontGlyphOutline Outline;
} CellFontGlyph;

typedef struct CellFontVertexesGlyphSubHeader {
	uint32_t size;
	float    SystemReserved[11];
} CellFontVertexesGlyphSubHeader;

typedef struct CellFontVertexesGlyphData {
	const uint32_t size;
	float SystemClosed[];
} CellFontVertexesGlyphData;

typedef struct CellFontVertexesGlyph {
	CellFontVertexesGlyphData      *data;
	CellFontVertexesGlyphSubHeader *subHeader;
} CellFontVertexesGlyph;

#define CellFontVertexesGlyph_getDataAddr(vGlyph) \
	((vGlyph) ? (vGlyph)->data : (CellFontVertexesGlyphData *)0)
#define CellFontVertexesGlyph_getDataSize(vGlyph) \
	(((vGlyph) && (vGlyph)->data) ? (vGlyph)->data->size : 0)
#define CellFontVertexesGlyph_getSubHeader(vGlyph) \
	((vGlyph) ? (vGlyph)->subHeader : (CellFontVertexesGlyphSubHeader *)0)

typedef void (*CellFontGetOutlineVertexCallback)(
	void *arg, int32_t contourN, int32_t vertexNumber,
	int32_t vertexAttr, float x, float y);

typedef struct CellFontGetOutlineVertexesIF {
	CellFontGetOutlineVertexCallback callback;
	void *arg;
} CellFontGetOutlineVertexesIF;

typedef struct CellFontGlyphBoundingBox {
	struct { float x, y; } Min;
	struct { float x, y; } Max;
} CellFontGlyphBoundingBox;

typedef struct CellFontKerning {
	float offsetX;
	float offsetY;
} CellFontKerning;

typedef struct CellFontGlyphStyle {
	struct { float widthPixel, heightPixel; } Scale;
	struct { float weight, slant; } Effect;
} CellFontGlyphStyle;

typedef struct CellFontRenderSurface {
	void    *buffer;
	int32_t  widthByte;
	int32_t  pixelSizeByte;
	int32_t  width, height;
	struct { uint32_t x0, y0, x1, y1; } Scissor;
} CellFontRenderSurface;

typedef struct CellFontImageTransInfo {
	uint8_t *Image;
	uint32_t imageWidthByte;
	uint32_t imageWidth;
	uint32_t imageHeight;
	void    *Surface;
	uint32_t surfWidthByte;
} CellFontImageTransInfo;

typedef struct CellFont {
	void *SystemReserved[64];
} CellFont;

typedef struct CellFontRendererConfig {
	struct {
		void     *buffer;
		uint32_t  initSize;
		uint32_t  maxSize;
		uint32_t  expandSize;
		uint32_t  resetSize;
	} BufferingPolicy;
} CellFontRendererConfig;

static inline void CellFontRendererConfig_initialize(
	CellFontRendererConfig *config)
{
	config->BufferingPolicy.buffer     = (void *)0;
	config->BufferingPolicy.initSize   = 0;
	config->BufferingPolicy.maxSize    = 0;
	config->BufferingPolicy.expandSize = 1;
	config->BufferingPolicy.resetSize  = 0;
}

static inline void CellFontRendererConfig_setAllocateBuffer(
	CellFontRendererConfig *config,
	uint32_t initSize, uint32_t maxSize)
{
	config->BufferingPolicy.buffer     = (void *)0;
	config->BufferingPolicy.initSize   = initSize;
	config->BufferingPolicy.maxSize    = maxSize;
	config->BufferingPolicy.expandSize = 1;
	config->BufferingPolicy.resetSize  = 0;
}

typedef struct CellFontRenderer {
	void *systemReserved[64];
} CellFontRenderer;

typedef struct CellFontGraphics {
	uint32_t graphicsType;
	uint32_t SystemClosed[];
} CellFontGraphics;

typedef struct CellFontGraphicsDrawContext {
	void *SystemReserved[64];
} CellFontGraphicsDrawContext;

/* Prototypes */

static inline int cellFontInit(CellFontConfig *config)
{
	extern void cellFontGetStubRevisionFlags(
		uint64_t *revisionFlags);
	extern int cellFontInitializeWithRevision(
		uint64_t, CellFontConfig *);
	uint64_t revisionFlags = 0LL;
	cellFontGetStubRevisionFlags(&revisionFlags);
	return cellFontInitializeWithRevision(revisionFlags, config);
}

int cellFontSetFontsetOpenMode(const CellFontLibrary *,
                               uint32_t openMode);
int cellFontSetFontOpenMode(const CellFontLibrary *,
                            uint32_t openMode);

#define CELL_FONT_OPEN_MODE_DEFAULT                  (0)
#define CELL_FONT_OPEN_MODE_IGNORE_VERTICAL_METRICS   (1 << 0)

int cellFontOpenFontset(const CellFontLibrary *,
                        CellFontType *, CellFont *);
int cellFontOpenFontsetOnMemory(const CellFontLibrary *,
                                CellFontType *, CellFont *);
int cellFontOpenFontFile(const CellFontLibrary *,
                         uint8_t *fontPath, uint32_t subNum,
                         int32_t uniqueID, CellFont *);
int cellFontOpenFontMemory(const CellFontLibrary *,
                           void *fontAddr, uint32_t fontSize,
                           uint32_t subNum, int32_t uniqueID,
                           CellFont *);
int cellFontOpenFontInstance(CellFont *org, CellFont *ins);

int cellFontGetLibrary(CellFont *, const CellFontLibrary **lib,
                       uint32_t *type);
int cellFontAdjustGlyphExpandBuffer(CellFont *, int32_t pointN,
                                    int32_t contourN);
int cellFontGetGlyphExpandBufferInfo(CellFont *, int32_t *pointN,
                                     int32_t *contourN);
int cellFontAdjustFontScaling(CellFont *, float fontScale);

int cellFontSetResolutionDpi(CellFont *, uint32_t hDpi,
                             uint32_t vDpi);
int cellFontSetScalePoint(CellFont *, float w, float h);
int cellFontSetScalePixel(CellFont *, float w, float h);
int cellFontSetEffectWeight(CellFont *, float effectWeight);
int cellFontSetEffectSlant(CellFont *, float effectSlant);

int cellFontGetResolutionDpi(CellFont *, uint32_t *hDpi,
                             uint32_t *vDpi);
int cellFontGetScalePoint(CellFont *, float *w, float *h);
int cellFontGetScalePixel(CellFont *, float *w, float *h);
int cellFontGetEffectWeight(CellFont *, float *effectWeight);
int cellFontGetEffectSlant(CellFont *, float *effectSlant);

int cellFontGetHorizontalLayout(CellFont *,
                                CellFontHorizontalLayout *layout);
int cellFontGetVerticalLayout(CellFont *,
                              CellFontVerticalLayout *layout);

int cellFontGetFontIdCode(CellFont *, uint32_t code,
                          uint32_t *fontId, uint32_t *fontcode);

int cellFontGetCharGlyphMetrics(CellFont *, uint32_t code,
                                CellFontGlyphMetrics *);
int cellFontGetCharGlyphMetricsVertical(CellFont *,
                                        uint32_t code,
                                        CellFontGlyphMetrics *);

int cellFontGetKerning(CellFont *, uint32_t preCode,
                       uint32_t code,
                       CellFontKerning *kerning);

int cellFontCreateRenderer(const CellFontLibrary *lib,
                           CellFontRendererConfig *config,
                           CellFontRenderer *renderer);

int cellFontBindRenderer(CellFont *, CellFontRenderer *);
int cellFontGetBindingRenderer(CellFont *,
                               CellFontRenderer **);

int cellFontSetupRenderScalePoint(CellFont *, float w, float h);
int cellFontSetupRenderScalePixel(CellFont *, float w, float h);
int cellFontSetupRenderEffectWeight(CellFont *,
                                    float additionalWeight);
int cellFontSetupRenderEffectSlant(CellFont *, float effectSlant);

int cellFontGetRenderScalePoint(CellFont *, float *w, float *h);
int cellFontGetRenderScalePixel(CellFont *, float *w, float *h);
int cellFontGetRenderEffectWeight(CellFont *,
                                  float *effectWeight);
int cellFontGetRenderEffectSlant(CellFont *,
                                 float *effectSlant);

int cellFontGetRenderCharGlyphMetrics(CellFont *,
                                      uint32_t code,
                                      CellFontGlyphMetrics *);
int cellFontGetRenderCharGlyphMetricsVertical(
	CellFont *cfEx, uint32_t code,
	CellFontGlyphMetrics *metrics);
int cellFontGetRenderScaledKerning(CellFont *,
                                   uint32_t preCode,
                                   uint32_t code,
                                   CellFontKerning *kerning);

int cellFontGenerateCharGlyph(CellFont *, uint32_t code,
                              CellFontGlyph **);
int cellFontGenerateCharGlyphVertical(CellFont *,
                                      uint32_t code,
                                      CellFontGlyph **);
int cellFontDeleteGlyph(CellFont *, CellFontGlyph *);
int cellFontDelete(const CellFontLibrary *, void *);

void cellFontRenderSurfaceInit(CellFontRenderSurface *,
                               void *buffer,
                               int bufWidthByte,
                               int pixelSizeByte,
                               int w, int h);
void cellFontRenderSurfaceSetScissor(CellFontRenderSurface *,
                                     int x0, int y0,
                                     uint32_t w, uint32_t h);

int cellFontRenderCharGlyphImage(CellFont *, uint32_t code,
                                 CellFontRenderSurface *,
                                 float x, float y,
                                 CellFontGlyphMetrics *,
                                 CellFontImageTransInfo *);
int cellFontRenderCharGlyphImageHorizontal(
	CellFont *, uint32_t code,
	CellFontRenderSurface *, float x, float y,
	CellFontGlyphMetrics *, CellFontImageTransInfo *);
int cellFontRenderCharGlyphImageVertical(
	CellFont *, uint32_t code,
	CellFontRenderSurface *, float x, float y,
	CellFontGlyphMetrics *, CellFontImageTransInfo *);

int cellFontUnbindRenderer(CellFont *);

int cellFontGlyphRenderImage(CellFontGlyph *,
                             CellFontGlyphStyle *,
                             CellFontRenderer *,
                             CellFontRenderSurface *surf,
                             float x, float y,
                             CellFontGlyphMetrics *,
                             CellFontImageTransInfo *);
int cellFontGlyphRenderImageHorizontal(
	CellFontGlyph *, CellFontGlyphStyle *,
	CellFontRenderer *,
	CellFontRenderSurface *surf,
	float x, float y,
	CellFontGlyphMetrics *,
	CellFontImageTransInfo *);
int cellFontGlyphRenderImageVertical(
	CellFontGlyph *, CellFontGlyphStyle *,
	CellFontRenderer *,
	CellFontRenderSurface *surf,
	float x, float y,
	CellFontGlyphMetrics *,
	CellFontImageTransInfo *);

int cellFontGlyphGetHorizontalShift(CellFontGlyph *,
                                    float *shiftX, float *shiftY);
int cellFontGlyphGetVerticalShift(CellFontGlyph *,
                                  float *shiftX, float *shiftY);
int cellFontGlyphGetScalePixel(CellFontGlyph *,
                               float *w, float *h);

#define CELL_FONT_GLYPH_OUTLINE_CONTROL_DISTANCE_DEFAULT (0.125f)

int cellFontGlyphGetOutlineControlDistance(
	CellFontGlyph *, float maxScale,
	float baseControlDistance,
	float *controlDistance);
int cellFontGlyphGetOutlineVertexes(
	CellFontGlyph *glyph, float controlDistance,
	CellFontGetOutlineVertexesIF *getIF,
	CellFontGlyphBoundingBox *bbox,
	uint32_t *vcount);
int cellFontGlyphGetVertexesGlyphSize(
	CellFontGlyph *glyph, float controlDistance,
	uint32_t *useSize);
int cellFontGlyphSetupVertexesGlyph(
	CellFontGlyph *glyph, float controlDistance,
	uint32_t *mappedBuf, uint32_t mappedBufSize,
	CellFontVertexesGlyph *vGlyph,
	uint32_t *dataSize);
int cellFontVertexesGlyphRelocate(
	CellFontVertexesGlyph *vGlyph,
	CellFontVertexesGlyph *vGlyph2,
	CellFontVertexesGlyphSubHeader *,
	uint32_t *localBuf, uint32_t copySize);

int cellFontDestroyRenderer(CellFontRenderer *renderer);
int cellFontCloseFont(CellFont *cf);
int cellFontClearFileCache(void);
int cellFontEndLibrary(const CellFontLibrary *lib);

int cellFontGraphicsSetupDrawContext(
	const CellFontGraphics *,
	CellFontGraphicsDrawContext *);
int cellFontGraphicsSetDrawType(
	CellFontGraphicsDrawContext *, uint32_t type);
int cellFontGraphicsGetDrawType(
	CellFontGraphicsDrawContext *, uint32_t *type);

#define CELL_FONT_GRAPHICS_DRAW_TYPE_MONO          (0)
#define CELL_FONT_GRAPHICS_DRAW_TYPE_COLOR         (1)
#define CELL_FONT_GRAPHICS_DRAW_TYPE_COLOR_REVERSE (2)

int cellFontGraphicsSetFontRGBA(
	CellFontGraphicsDrawContext *,
	float fontRGBA[4]);
int cellFontGraphicsGetFontRGBA(
	CellFontGraphicsDrawContext *,
	float fontRGBA[4]);
int cellFontGraphicsSetLineRGBA(
	CellFontGraphicsDrawContext *,
	float lineRGBA[4]);
int cellFontGraphicsGetLineRGBA(
	CellFontGraphicsDrawContext *,
	float lineRGBA[4]);
int cellFontGraphicsSetScalePixel(
	CellFontGraphicsDrawContext *, float w, float h);
int cellFontGraphicsGetScalePixel(
	CellFontGraphicsDrawContext *, float *w,
	float *h);

int cellFontEndGraphics(const CellFontGraphics *);
int cellFontEnd(void);

/* Stub-revision helpers (declared in oracle wrapper, materialized
 * for link-and-resolve sample visibility) */
extern void cellFontGetStubRevisionFlags(
	uint64_t *revisionFlags);

int cellFontInitializeWithRevision(
	uint64_t revisionFlags, CellFontConfig *config);
int cellFontGetRevisionFlags(
	const CellFontLibrary *lib, uint64_t *revisionFlags);
int cellFontGetInitializedRevisionFlags(
	const CellFontLibrary *lib, uint64_t *revisionFlags);

#ifdef __cplusplus
}
#endif

#endif
