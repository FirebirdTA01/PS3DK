#include <string.h>

#include <cell/error.h>
#include <cell/fontGcm.h>

static uint32_t s_font_graphics_gcm_storage[2];
static uint32_t s_packet_mode;

int cellFontInitGraphicsGcm(CellFontInitGraphicsConfigGcm *config,
                            const CellFontGraphics **graphics)
{
    (void)config;
    s_font_graphics_gcm_storage[0] = CELL_FONT_GRAPHICS_TYPE_GCM;
    if (graphics) {
        *graphics = (const CellFontGraphics *)s_font_graphics_gcm_storage;
    }
    return CELL_OK;
}

int cellFontGraphicsSetGcmPacketMode(CellFontGraphicsDrawContext *drawContext,
                                     uint32_t packetMode)
{
    (void)drawContext;
    s_packet_mode = packetMode;
    return CELL_OK;
}

int cellFontGraphicsGetGcmPacketMode(CellFontGraphicsDrawContext *drawContext,
                                     uint32_t *packetMode)
{
    (void)drawContext;
    if (packetMode) {
        *packetMode = s_packet_mode;
    }
    return CELL_OK;
}

int cellFontRenderSurfaceGcmInit(CellFontRenderSurfaceGcm *renderSurfGcm,
                                 CellGcmSurface *gcmSurface)
{
    (void)gcmSurface;
    if (renderSurfGcm) {
        memset(renderSurfGcm, 0, sizeof(*renderSurfGcm));
    }
    return CELL_OK;
}

void cellFontRenderSurfaceGcmSetScissor(CellFontRenderSurfaceGcm *renderSurfGcm,
                                        int x0, int y0, uint32_t w,
                                        uint32_t h)
{
    (void)renderSurfGcm;
    (void)x0;
    (void)y0;
    (void)w;
    (void)h;
}

int cellFontGraphicsGcmSetClearSurface(CellGcmContextData *contextData,
                                       CellFontRenderSurfaceGcm *renderSurfGcm,
                                       uint32_t clearEnable,
                                       CellFontGraphicsDrawContext *drawContext)
{
    (void)contextData;
    (void)renderSurfGcm;
    (void)clearEnable;
    (void)drawContext;
    return CELL_OK;
}

int cellFontGraphicsGcmSetDrawGlyph(CellGcmContextData *contextData,
                                    CellFontRenderSurfaceGcm *renderSurfGcm,
                                    float sx, float sy,
                                    CellFontVertexesGlyph *outVertexesGlyph,
                                    CellFontGraphicsDrawContext *drawContext)
{
    (void)contextData;
    (void)renderSurfGcm;
    (void)sx;
    (void)sy;
    (void)outVertexesGlyph;
    (void)drawContext;
    return CELL_OK;
}
