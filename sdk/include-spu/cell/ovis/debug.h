/* cell/ovis/debug.h - the overlay table in local store.
 *
 * _ovly_table and _novlys are laid down by the overlay linker script (see
 * cellOvisMkLdscript / cellOvisConfigAuto); a debugger reads them, with a
 * breakpoint on _ovly_debug_event, to know which section is resident. */
#ifndef PS3TC_CELL_OVIS_DEBUG_H
#define PS3TC_CELL_OVIS_DEBUG_H

#include <spu_intrinsics.h>
#include <stdint.h>
#include <cell/ovis/types.h>

extern _ovly_table_t _ovly_table[];
extern unsigned long _novlys;

#endif /* PS3TC_CELL_OVIS_DEBUG_H */
