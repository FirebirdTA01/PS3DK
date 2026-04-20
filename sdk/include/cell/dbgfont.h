/*
 * PS3 Custom Toolchain — <cell/dbgfont.h>
 *
 * Tier-1 stub for the reference libdbgfont — the on-screen debug text
 * overlay (FPS counter, timing stats, etc.).  Implementing the
 * real NV40 renderer (font atlas upload, VP/FP shader pair,
 * per-glyph-quad vertex stream) is follow-up work; for now the
 * printf-family entry points forward to host stdout so cell-SDK
 * sample code that calls cellDbgFontPrintf still produces visible
 * output — it just lands in RPCS3's TTY.log rather than on top
 * of the frame.  cellDbgFontDrawGcm stays a no-op since we've
 * already flushed each line when its Printf ran; there's no
 * accumulated per-frame state to emit.
 *
 * The struct layout / constant values below match a 3.55-4.75-era
 * cell SDK's <cell/dbgfont.h>, so when the real runtime lands it
 * can slot in without touching sample code.  See
 * docs/known-issues.md for the tier-2 renderer plan.
 */

#ifndef PS3TC_CELL_DBGFONT_H
#define PS3TC_CELL_DBGFONT_H

#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
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
{
    /* No accumulated state in tier-1; Printf/Puts already flushed.
     * Tier-2 (real on-screen renderer) will emit per-frame quads here. */
    return 0;
}

/* Printf / Puts: redirect to stdout with a tagged prefix so the
 * sample's intended debug-overlay text lands in RPCS3's TTY.log.
 * Position is clamped-printed as two decimals since the reference
 * runtime passes normalized [0..1] screen coords; color is kept hex
 * for easy tie-back to CELL_GCM_UTIL_COLOR / ARGB literals in the
 * sample source.  Returns the number of bytes written (the reference
 * function returns `int32_t status`, 0 == OK — positive byte counts
 * still register as "ok" for every caller we've seen). */
static inline int32_t cellDbgFontPuts(float x, float y, float scale,
                                      uint32_t color, const char *s)
{
    if (!s) return 0;
    int n = printf("[dbgfont %.2f,%.2f s=%.2f #%08x] %s\n",
                   x, y, scale, (unsigned)color, s);
    fflush(stdout);
    return (n < 0) ? 0 : n;
}

static inline int32_t cellDbgFontPrintf(float x, float y, float scale,
                                        uint32_t color, const char *fmt, ...)
{
    if (!fmt) return 0;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (m < 0) return 0;
    if ((size_t)m >= sizeof(buf)) {
        /* vsnprintf truncated; null-terminator is already in place. */
    }
    return cellDbgFontPuts(x, y, scale, color, buf);
}

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_DBGFONT_H */
