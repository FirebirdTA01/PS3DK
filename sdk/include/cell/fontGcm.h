/*
 * PS3 Custom Toolchain -- <cell/fontGcm.h>
 *
 * Independent GCM-renderer surface for the cellFont library.
 * Declares the 7 API entry points that bind the font renderer
 * to the RSX GCM command stream.
 */

#ifndef __PS3DK_CELL_FONTGCM_H__
#define __PS3DK_CELL_FONTGCM_H__

#include <stdint.h>
#include <cell/font.h>
#include <cell/gcm.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ---------------------------------------------------------------------------
 *  Initialization config & helper
 * ---------------------------------------------------------------------------
 */

typedef struct CellFontInitGraphicsConfigGcm {
	uint32_t configType;

	struct {
		void    *address;
		uint32_t size;
	} GraphicsMemory;

	struct {
		void    *address;
		uint32_t size;
	} MappedMainMemory;

	struct {
		int16_t slotNumber;
		int16_t slotCount;
	} VertexShader;
} CellFontInitGraphicsConfigGcm;

#define CELL_FONT_INIT_GRAPHICS_CONFIG_TYPE_DEFAULT  (0)
#define CELL_FONT_VERTEX_SHADER_SLOT_COUNT_DEFAULT   (63)

static inline void
CellFontInitGraphicsConfigGcm_initialize(CellFontInitGraphicsConfigGcm *configGcm)
{
	configGcm->configType              = CELL_FONT_INIT_GRAPHICS_CONFIG_TYPE_DEFAULT;
	configGcm->GraphicsMemory.address  = (void *)0;
	configGcm->GraphicsMemory.size     = 0;
	configGcm->MappedMainMemory.address = (void *)0;
	configGcm->MappedMainMemory.size   = 0;
	configGcm->VertexShader.slotNumber = 0;
	configGcm->VertexShader.slotCount  = CELL_FONT_VERTEX_SHADER_SLOT_COUNT_DEFAULT;
}

/*
 * ---------------------------------------------------------------------------
 *  GCM render-surface
 * ---------------------------------------------------------------------------
 */

typedef struct CellFontRenderSurfaceGcm {
	CellFontRenderSurface RenderSurface;
	void *SystemClosed[96 - sizeof(CellFontRenderSurface) / sizeof(void *)];
} CellFontRenderSurfaceGcm;

#define CELL_FONT_GRAPHICS_TYPE_GCM  (1)

/*
 * ---------------------------------------------------------------------------
 *  Packet-mode constants
 * ---------------------------------------------------------------------------
 */

#define CELL_FONT_GRAPHICS_GCM_PACKET_WRITE_DISABLE  (0x00008000)
#define CELL_FONT_GRAPHICS_GCM_PACKET_WRITE_ENABLE   (0x80000000)
#define CELL_FONT_GRAPHICS_GCM_PACKET_CALL_DISABLE   (0x00000001)
#define CELL_FONT_GRAPHICS_GCM_PACKET_CALL_ENABLE    (0x00010000)

/*
 * ---------------------------------------------------------------------------
 *  API
 * ---------------------------------------------------------------------------
 */

int cellFontInitGraphicsGcm(
	CellFontInitGraphicsConfigGcm *config,
	const CellFontGraphics       **graphics);

int cellFontGraphicsSetGcmPacketMode(
	CellFontGraphicsDrawContext *drawContext,
	uint32_t                     packetMode);

int cellFontGraphicsGetGcmPacketMode(
	CellFontGraphicsDrawContext *drawContext,
	uint32_t                    *packetMode);

int cellFontRenderSurfaceGcmInit(
	CellFontRenderSurfaceGcm *renderSurfGcm,
	CellGcmSurface           *gcmSurface);

void cellFontRenderSurfaceGcmSetScissor(
	CellFontRenderSurfaceGcm *renderSurfGcm,
	int                       x0,
	int                       y0,
	uint32_t                  w,
	uint32_t                  h);

int cellFontGraphicsGcmSetClearSurface(
	CellGcmContextData            *contextData,
	CellFontRenderSurfaceGcm      *renderSurfGcm,
	uint32_t                       clearEnable,
	CellFontGraphicsDrawContext   *drawContext);

int cellFontGraphicsGcmSetDrawGlyph(
	CellGcmContextData          *contextData,
	CellFontRenderSurfaceGcm    *renderSurfGcm,
	float                        sx,
	float                        sy,
	CellFontVertexesGlyph       *outVertexesGlyph,
	CellFontGraphicsDrawContext *drawContext);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_FONTGCM_H__ */
