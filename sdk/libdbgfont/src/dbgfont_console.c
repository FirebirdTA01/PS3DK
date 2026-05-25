/*
 * libdbgfont -- virtual text console subsystem.
 *
 * Implements the cellDbgFontConsole* API declared in
 * sdk/include/cell/dbgfont.h.  Each console owns a ring buffer of
 * text lines; cellDbgFontConsoleVprintf / Printf append formatted
 * output to the current line and advance the write cursor.
 *
 * At draw time (cellDbgFontDrawGcm), every active console iterates
 * its ring buffer from oldest to newest line and renders each via
 * the existing cellDbgFontPuts path so the text shares the same
 * font atlas and shader as the overlay API.
 *
 * Slot 0 is reserved as CELL_DBGFONT_STDOUT_ID and is auto-created
 * by cellDbgFontInitGcm with sensible defaults (full-width, white).
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <cell/dbgfont.h>

/* ----- console pool sizing ---------------------------------------- */
#define CONSOLE_MAX         8
#define CONSOLE_LINE_LEN    256
#define CONSOLE_LINE_COUNT  64

/* ----- per-console state ------------------------------------------ */
typedef struct {
    int                     active;
    CellDbgFontConsoleConfig cfg;
    char                    lines[CONSOLE_LINE_COUNT][CONSOLE_LINE_LEN];
    int                     cursor;   /* next line index to overwrite */
    int                     written;  /* total lines written (for wrap) */
} ConsoleSlot;

static ConsoleSlot g_consoles[CONSOLE_MAX];

/* ----- helpers ---------------------------------------------------- */

static int slot_valid(CellDbgFontConsoleId id)
{
    return (id >= 0 && id < CONSOLE_MAX && g_consoles[id].active);
}

static void slot_clear(int slot)
{
    memset(&g_consoles[slot], 0, sizeof(ConsoleSlot));
}

/* ----- public API ------------------------------------------------- */

int cellDbgFontConsoleOpen(const CellDbgFontConsoleConfig *cfg)
{
    if (!cfg) return CELL_DBGFONT_CONSOLE_INVALID_ID;

    /* Find a free slot.  Slot 0 is reserved for stdout -- do not
     * hand it out through Open.  A caller can still use STDOUT_ID
     * directly without opening. */
    for (int i = 1; i < CONSOLE_MAX; i++) {
        if (!g_consoles[i].active) {
            slot_clear(i);
            g_consoles[i].active = 1;
            g_consoles[i].cfg    = *cfg;
            g_consoles[i].cursor = 0;
            g_consoles[i].written = 0;
            return (CellDbgFontConsoleId)i;
        }
    }
    return CELL_DBGFONT_CONSOLE_INVALID_ID;
}

int cellDbgFontConsoleClose(CellDbgFontConsoleId id)
{
    if (!slot_valid(id)) return -1;
    slot_clear((int)id);
    return 0;
}

int cellDbgFontConsoleVprintf(CellDbgFontConsoleId id,
                              const char *fmt, va_list ap)
{
    if (!slot_valid(id) || !fmt) return -1;

    ConsoleSlot *s = &g_consoles[(int)id];
    int n = vsnprintf(s->lines[s->cursor], CONSOLE_LINE_LEN, fmt, ap);

    /* Advance write cursor -- ring-buffer wrap. */
    s->cursor = (s->cursor + 1) % CONSOLE_LINE_COUNT;
    s->written++;
    return n;
}

int cellDbgFontConsolePrintf(CellDbgFontConsoleId id, const char *fmt, ...)
{
    if (!slot_valid(id) || !fmt) return -1;

    va_list ap;
    va_start(ap, fmt);
    int n = cellDbgFontConsoleVprintf(id, fmt, ap);
    va_end(ap);
    return n;
}

/* ----- integration with the overlay renderer -----------------------
 * Called from cellDbgFontDrawGcm before batching the overlay quads.
 * Iterates all active console slots and renders each line of text
 * oldest-to-newest so the most recent output is at the bottom. */

void _cellDbgFontDrawConsoles(void)
{
    /* Kernel of a glyph height in normalised screen coords at the
     * reference 720p resolution -- one 10 px line (= glyph + 1 px
     * padding) mapped from [0..720] to [0..1]. */
    static const float kLineH = 10.0f / 720.0f;

    /* Iterate slots in order so slot 0 (stdout) draws first. */
    for (int s_idx = 0; s_idx < CONSOLE_MAX; s_idx++) {
        ConsoleSlot *s = &g_consoles[s_idx];
        if (!s->active) continue;

        float scale  = s->cfg.scale;
        uint32_t color  = s->cfg.color;
        float left   = s->cfg.posLeft;
        float top    = s->cfg.posTop;
        float line_h = kLineH * scale;

        int total = s->written;
        int count = (total < CONSOLE_LINE_COUNT) ? total
                                                  : CONSOLE_LINE_COUNT;

        /* Walk ring buffer from oldest (cursor) to newest, wrapping
         * so the last-written line renders at the bottom.  We render
         * at most `cnsHeight` visible lines from the bottom of the
         * ring. */
        int start  = (total <= CONSOLE_LINE_COUNT) ? 0
                     : (s->cursor % CONSOLE_LINE_COUNT);
        int visible = (s->cfg.cnsHeight > 0 && s->cfg.cnsHeight < count)
                      ? (int)s->cfg.cnsHeight : count;
        int skip = count - visible;
        int idx = (start + skip) % CONSOLE_LINE_COUNT;

        for (int row = 0; row < visible; row++) {
            float cy = top + (float)row * line_h;
            cellDbgFontPuts(left, cy, scale, color,
                            s->lines[idx]);
            idx = (idx + 1) % CONSOLE_LINE_COUNT;
        }
    }
}

/* ----- stdout auto-creation ---------------------------------------
 * Called by cellDbgFontInitGcm after the renderer is up.  Creates a
 * default console at slot 0 with sensible defaults so
 * CELL_DBGFONT_STDOUT_ID is always usable. */

void _cellDbgFontInitStdoutConsole(void)
{
    slot_clear(0);
    g_consoles[0].active = 1;
    g_consoles[0].cfg.posLeft   = 0.0f;
    g_consoles[0].cfg.posTop    = 0.0f;
    g_consoles[0].cfg.cnsWidth  = 80;
    g_consoles[0].cfg.cnsHeight = 24;
    g_consoles[0].cfg.scale     = 1.0f;
    g_consoles[0].cfg.color     = 0xffffffffu; /* white */
    g_consoles[0].cursor        = 0;
    g_consoles[0].written       = 0;
}
