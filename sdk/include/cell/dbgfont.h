/*
 * PS3 Custom Toolchain — <cell/dbgfont.h>
 *
 * Declarations for the cellDbgFont* on-screen text overlay API.
 * Implementation lives in libdbgfont.a (sdk/libdbgfont/).  Samples
 * that use this API must link `-ldbgfont` in addition to the usual
 * `-lgcm_cmd -lrsx …` set.
 *
 * Semantics (matches the cell SDK 475.001 <cell/dbgfont.h>):
 *
 *   InitGcm(cfg)        Init once per process.  `cfg` describes the
 *                       caller-provided buffer that holds the fragment
 *                       program, font atlas, and vertex pool in that
 *                       order.  Size the buffer as
 *                           CELL_DBGFONT_FRAGMENT_SIZE
 *                         + CELL_DBGFONT_TEXTURE_SIZE
 *                         + 1024 * CELL_DBGFONT_VERTEX_SIZE      (typical)
 *                       and align 128 bytes.
 *   Puts / Printf       Stage one string at normalized [0..1] screen
 *                       coords `(x,y)` (y=0 is top) with glyph
 *                       `scale` and ARGB `color`.  No FIFO traffic —
 *                       only vertex accumulation.
 *   DrawGcm             Emit the batched quads + the render state
 *                       (alpha-blend on, depth-test off).  Call once
 *                       per frame after all Puts/Printf for that
 *                       frame have been staged.
 *   ExitGcm             Releases the renderer's handle to the caller
 *                       buffer.  Caller owns the memory.
 *
 * Tier-2 limitation: the API doesn't take a screen resolution so
 * the NDC conversion assumes a 1280x720 reference (see dbgfont.c);
 * text will visually scale roughly correctly on other resolutions
 * but not pixel-exact.  A follow-up surface-aware variant can take
 * resolution from the active CellGcmSurface.
 */

#ifndef PS3TC_CELL_DBGFONT_H
#define PS3TC_CELL_DBGFONT_H

#include <stdint.h>
#include <ppu-types.h>        /* sys_addr_t */

#ifdef __cplusplus
extern "C" {
#endif

#define CELL_DBGFONT_FRAGMENT_SIZE    512
#define CELL_DBGFONT_TEXTURE_SIZE     (256 * 256)
#define CELL_DBGFONT_VERTEX_SIZE      32
#define CELL_DBGFONT_VERTEX_LOCAL     0x00000001u
#define CELL_DBGFONT_TEXTURE_LOCAL    0x00000002u
#define CELL_DBGFONT_VERTEX_MAIN      0x00000004u
#define CELL_DBGFONT_TEXTURE_MAIN     0x00000008u
#define CELL_DBGFONT_SYNC_ON           0x00000010u
#define CELL_DBGFONT_SYNC_OFF           0x00000000u
#define CELL_DBGFONT_VIEWPORT_ON        0x00000020u
#define CELL_DBGFONT_MINFILTER_LINEAR   0x00000040u
#define CELL_DBGFONT_MINFILTER_NEAREST  0x00000000u
#define CELL_DBGFONT_MAGFILTER_LINEAR   0x00000080u
#define CELL_DBGFONT_MAGFILTER_NEAREST  0x00000000u

typedef struct CellDbgFontConfigGcm {
    sys_addr_t localBufAddr;
    uint32_t   localBufSize;
    sys_addr_t mainBufAddr;
    uint32_t   mainBufSize;
    uint16_t   screenWidth;
    uint16_t   screenHeight;
    uint32_t   option;
} CellDbgFontConfigGcm;

int32_t cellDbgFontInitGcm(const CellDbgFontConfigGcm *cfg);
int32_t cellDbgFontExitGcm(void);
int32_t cellDbgFontDrawGcm(void);
int32_t cellDbgFontPuts  (float x, float y, float scale, uint32_t color,
                          const char *s);
int32_t cellDbgFontPrintf(float x, float y, float scale, uint32_t color,
                          const char *fmt, ...)
                          __attribute__((format(printf, 5, 6)));

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_DBGFONT_H */
