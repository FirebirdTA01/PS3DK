/*
 * PS3 Custom Toolchain — <cell/dbgfont.h>
 *
 * Stub shim for Sony's libdbgfont runtime — the on-screen debug
 * text overlay (FPS counter, perf stats, etc.).  We don't ship a
 * dbgfont implementation yet; these inline stubs keep Sony sample
 * source that includes <cell/dbgfont.h> compilable and linkable
 * against our SDK, at the cost of the debug text not actually
 * being drawn.  Every function returns success; the sample's main
 * rendering still happens.
 *
 * Replace with a real implementation (vertex + texture upload to
 * local memory, font atlas, text emitter) when we want parity with
 * Sony's overlay.  The struct layout / constants here come from
 * Sony SDK 475.001's <cell/dbgfont.h> so an unchanged runtime can
 * slot in later.
 */

#ifndef PS3TC_CELL_DBGFONT_H
#define PS3TC_CELL_DBGFONT_H

#include <stdint.h>
#include <sys/process.h>      /* sys_addr_t */

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
#define CELL_DBGFONT_SYNC_ON          0x00000010u

typedef struct CellDbgFontConfigGcm {
    sys_addr_t localBufAddr;
    uint32_t   localBufSize;
    sys_addr_t mainBufAddr;
    uint32_t   mainBufSize;
    uint32_t   option;
} CellDbgFontConfigGcm;

static inline int32_t cellDbgFontInitGcm(const CellDbgFontConfigGcm *cfg)
{ (void)cfg; return 0; }

static inline int32_t cellDbgFontExitGcm(void)
{ return 0; }

static inline int32_t cellDbgFontDrawGcm(void)
{ return 0; }

static inline int32_t cellDbgFontPrintf(float x, float y, float scale,
                                        uint32_t color, const char *fmt, ...)
{ (void)x; (void)y; (void)scale; (void)color; (void)fmt; return 0; }

static inline int32_t cellDbgFontPuts(float x, float y, float scale,
                                      uint32_t color, const char *s)
{ (void)x; (void)y; (void)scale; (void)color; (void)s; return 0; }

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_DBGFONT_H */
