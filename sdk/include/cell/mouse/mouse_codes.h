/* cell/mouse/mouse_codes.h - mouse constants under their reference names:
 * info and tablet-mode flags, button bits, limits.  Included by
 * <cell/mouse.h>. */
#ifndef PS3TC_CELL_MOUSE_MOUSE_CODES_H
#define PS3TC_CELL_MOUSE_MOUSE_CODES_H

#include <stdint.h>   /* CellMouseInfoTablet */

#define CELL_MAX_MICE                                127
#define CELL_MOUSE_INFO_INTERCEPTED                  (1 << 0)
/* cellMouseInfoTabletMode: whether the device can act as a tablet, and the
 * mode it is in (CELL_MOUSE_INFO_TABLET_*). */
typedef struct CellMouseInfoTablet {
  uint32_t is_supported;
  uint32_t mode;
} CellMouseInfoTablet;

#define CELL_MOUSE_INFO_TABLET_NOT_SUPPORTED         0
#define CELL_MOUSE_INFO_TABLET_SUPPORTED             1
#define CELL_MOUSE_INFO_TABLET_MOUSE_MODE            1
#define CELL_MOUSE_INFO_TABLET_TABLET_MODE           2
#define CELL_MOUSE_BUTTON_1                          (1 << 0)
#define CELL_MOUSE_BUTTON_2                          (1 << 1)
#define CELL_MOUSE_BUTTON_3                          (1 << 2)
#define CELL_MOUSE_BUTTON_4                          (1 << 3)
#define CELL_MOUSE_BUTTON_5                          (1 << 4)
#define CELL_MOUSE_BUTTON_6                          (1 << 5)
#define CELL_MOUSE_BUTTON_7                          (1 << 6)
#define CELL_MOUSE_BUTTON_8                          (1 << 7)
#define CELL_MOUSE_MAX_DATA_LIST_NUM                 8

#endif /* PS3TC_CELL_MOUSE_MOUSE_CODES_H */
