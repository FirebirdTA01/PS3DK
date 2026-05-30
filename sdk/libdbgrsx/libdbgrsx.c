#include <string.h>

#include <cell/error.h>
#include <cell/dbgrsx.h>

static void set_u32(uint32_t *value, uint32_t result)
{
    if (value) {
        *value = result;
    }
}

static void set_i32(int32_t *value, int32_t result)
{
    if (value) {
        *value = result;
    }
}

int32_t cellDbgRsxInit(void) { return CELL_OK; }
int32_t cellDbgRsxExit(void) { return CELL_OK; }

int32_t cellDbgRsxGetTileRegion(uint8_t index, CellDbgRsxTileRegion *region)
{
    (void)index;
    if (region) {
        memset(region, 0, sizeof(*region));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullRegion(uint8_t index, CellDbgRsxZcullRegion *zcRegion)
{
    (void)index;
    if (zcRegion) {
        memset(zcRegion, 0, sizeof(*zcRegion));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullStatus(uint8_t index, CellDbgRsxZcullStatus *zcStatus)
{
    (void)index;
    if (zcStatus) {
        memset(zcStatus, 0, sizeof(*zcStatus));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullMatch(CellDbgRsxZcullMatch *zcMatch)
{
    if (zcMatch) {
        memset(zcMatch, 0, sizeof(*zcMatch));
    }
    return CELL_OK;
}

uint32_t cellDbgRsxGetZcullNearRamSize(uint32_t pixel_width, uint32_t pixel_height)
{
    (void)pixel_width;
    (void)pixel_height;
    return 0;
}

uint32_t cellDbgRsxGetZcullFarRamSize(uint32_t pixel_width, uint32_t pixel_height)
{
    (void)pixel_width;
    (void)pixel_height;
    return 0;
}

uint32_t cellDbgRsxGetZcullStencilRamSize(uint32_t pixel_width,
                                          uint32_t pixel_height,
                                          uint32_t antialias)
{
    (void)pixel_width;
    (void)pixel_height;
    (void)antialias;
    return 0;
}

int32_t cellDbgRsxGetZcullNearRamTextureSize(uint32_t pixel_width,
                                             uint32_t pixel_height,
                                             uint32_t *tex_width,
                                             uint32_t *tex_height,
                                             uint32_t *tex_format)
{
    set_u32(tex_width, pixel_width);
    set_u32(tex_height, pixel_height);
    set_u32(tex_format, 0);
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullFarRamTextureSize(uint32_t pixel_width,
                                            uint32_t pixel_height,
                                            uint32_t *tex_width,
                                            uint32_t *tex_height,
                                            uint32_t *tex_format)
{
    set_u32(tex_width, pixel_width);
    set_u32(tex_height, pixel_height);
    set_u32(tex_format, 0);
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullStencilRamTextureSize(uint32_t pixel_width,
                                                uint32_t pixel_height,
                                                uint32_t antialias,
                                                uint32_t *tex_width,
                                                uint32_t *tex_height,
                                                uint32_t *tex_format)
{
    (void)antialias;
    set_u32(tex_width, pixel_width);
    set_u32(tex_height, pixel_height);
    set_u32(tex_format, 0);
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullNearRam(uint16_t *data, uint32_t pixel_width,
                                  uint32_t pixel_height, uint32_t zcstart,
                                  uint32_t zdir, uint32_t zformat)
{
    (void)data;
    (void)pixel_width;
    (void)pixel_height;
    (void)zcstart;
    (void)zdir;
    (void)zformat;
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullFarRam(uint16_t *data, uint32_t pixel_width,
                                 uint32_t pixel_height, uint32_t zcstart,
                                 uint32_t zdir, uint32_t zformat)
{
    (void)data;
    (void)pixel_width;
    (void)pixel_height;
    (void)zcstart;
    (void)zdir;
    (void)zformat;
    return CELL_OK;
}

int32_t cellDbgRsxGetZcullStencilRam(uint8_t *data, uint32_t pixel_width,
                                     uint32_t pixel_height, uint32_t zcstart,
                                     uint32_t antialias)
{
    (void)data;
    (void)pixel_width;
    (void)pixel_height;
    (void)zcstart;
    (void)antialias;
    return CELL_OK;
}

int32_t cellDbgRsxEnableCursor(const CellDbgRsxCursor *cursor,
                               const uint32_t x, const uint32_t y)
{
    (void)cursor;
    (void)x;
    (void)y;
    return CELL_OK;
}

int32_t cellDbgRsxDisableCursor(void) { return CELL_OK; }

int32_t cellDbgRsxSetCursorPosition(const uint32_t x, const uint32_t y)
{
    (void)x;
    (void)y;
    return CELL_OK;
}

int32_t cellDbgRsxGetCursorStatus(int32_t *status)
{
    set_i32(status, 0);
    return CELL_OK;
}

int32_t cellDbgRsxSetCursorDefaultValues(CellDbgRsxCursor *cursor)
{
    if (cursor) {
        memset(cursor, 0, sizeof(*cursor));
        cursor->magnification_factor = CELL_DBG_RSX_CURSOR_MAGNIFICATION_FACTOR_1X;
    }
    return CELL_OK;
}

int32_t cellDbgRsxSetCursorBitmap(CellDbgRsxCursor *cursor,
                                  const uint8_t *source_bitmap)
{
    (void)source_bitmap;
    if (cursor) {
        memset(cursor->bitmap, 0, sizeof(cursor->bitmap));
    }
    return CELL_OK;
}

int32_t cellDbgRsxSetCursorPalette(CellDbgRsxCursor *cursor,
                                   const uint8_t *source_palette)
{
    (void)source_palette;
    if (cursor) {
        memset(cursor->palette, 0, sizeof(cursor->palette));
    }
    return CELL_OK;
}

int32_t cellDbgRsxSetCursorMagnificationFactor(CellDbgRsxCursor *cursor,
                                               const uint32_t magnification_factor)
{
    if (cursor) {
        cursor->magnification_factor = magnification_factor;
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetInterruptErrorStatus(uint32_t *status)
{
    set_u32(status, 0);
    return CELL_OK;
}

int32_t cellDbgRsxGetInterruptFifoErrorStatus(uint32_t *status)
{
    set_u32(status, 0);
    return CELL_OK;
}

int32_t cellDbgRsxGetInterruptIoifErrorStatus(uint32_t *status)
{
    set_u32(status, 0);
    return CELL_OK;
}

int32_t cellDbgRsxGetInterruptGraphicsErrorStatus(uint64_t *status)
{
    if (status) {
        *status = 0;
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetInterruptMiscErrorStatus(uint32_t *status)
{
    set_u32(status, 0);
    return CELL_OK;
}

int32_t cellDbgRsxGetFifoCacheIndex(CellDbgRsxFifoCache *cache, int32_t index)
{
    (void)index;
    if (cache) {
        memset(cache, 0, sizeof(*cache));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetFifoCache(CellDbgRsxFifoCache *cache)
{
    if (cache) {
        memset(cache, 0, sizeof(*cache));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetGraphicsFifo(CellDbgRsxGraphicsFifo *fifo)
{
    if (fifo) {
        memset(fifo, 0, sizeof(*fifo));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetFifoErrorState(int32_t id, CellDbgRsxErrorState *state)
{
    (void)id;
    if (state) {
        memset(state, 0, sizeof(*state));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetIoifErrorState(int32_t id, CellDbgRsxErrorState *state)
{
    (void)id;
    if (state) {
        memset(state, 0, sizeof(*state));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetGraphicsErrorState(int32_t id, CellDbgRsxErrorState *state)
{
    (void)id;
    if (state) {
        memset(state, 0, sizeof(*state));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetGraphicsLaunchCheckState(uint32_t id,
                                              CellDbgRsxErrorState *state)
{
    (void)id;
    if (state) {
        memset(state, 0, sizeof(*state));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetGraphicsMethodCheckState(uint32_t addr, uint32_t data,
                                              CellDbgRsxMethodCheckState *state)
{
    (void)addr;
    (void)data;
    if (state) {
        memset(state, 0, sizeof(*state));
    }
    return CELL_OK;
}

int32_t cellDbgRsxGetGraphicsBundleState(int32_t id, uint32_t *data)
{
    (void)id;
    if (data) {
        memset(data, 0, sizeof(uint32_t) * 32);
    }
    return CELL_OK;
}

int32_t cellDbgRsxExtractGcmMethod(const uint32_t *gcm_method, uint32_t index,
                                   CellDbgRsxFifoCache *fifo_cache)
{
    (void)gcm_method;
    (void)index;
    if (fifo_cache) {
        memset(fifo_cache, 0, sizeof(*fifo_cache));
    }
    return CELL_OK;
}

int32_t _cellDbgRsxFunc1(void) { return CELL_OK; }
int32_t _cellDbgRsxFunc2(void) { return CELL_OK; }
int32_t _cellDbgRsxFunc3(void) { return CELL_OK; }
int32_t _cellDbgRsxFunc4(void) { return CELL_OK; }
int32_t _cellDbgRsxFunc5(void) { return CELL_OK; }
int32_t _cellDbgRsxFunc6(void) { return CELL_OK; }
