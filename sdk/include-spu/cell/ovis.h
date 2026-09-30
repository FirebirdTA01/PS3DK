/* cell/ovis.h - libovis, SPU code overlays (SPU side).
 *
 * Code that does not fit in local store at once is split into overlay
 * sections sharing an LS range; libovis DMAs the one needed from the
 * overlay table the PPU built in main memory.  Two ways to use it:
 *   manual - list the sections in an XML file, generate the linker script
 *            with cellOvisMkLdscript, map with <cell/ovis/mapper.h>;
 *   auto   - cellOvisConfigAuto wraps every function of the overlay
 *            objects so a call maps its section (<cell/ovis/auto.h>).
 * Link -lovis. */
#ifndef PS3TC_CELL_OVIS_SPU_H
#define PS3TC_CELL_OVIS_SPU_H

#include <cell/ovis/debug.h>
#include <cell/ovis/auto.h>
#include <cell/ovis/mapper.h>
#include <cell/ovis/error.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPURS trace of overlay mapping; accepted and ignored (no trace buffer). */
void cellOvisEnableSpursTrace(int enable);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_OVIS_SPU_H */
