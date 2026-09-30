/* cell/daisy/error.h - libdaisy error codes. */
#ifndef PS3TC_CELL_DAISY_ERROR_H
#define PS3TC_CELL_DAISY_ERROR_H

#include <cell/error.h>

#define CELL_DAISY_ERROR_NO_BEGIN             CELL_ERROR_CAST(0x80410501)  /* end without a begin */
#define CELL_DAISY_ERROR_INVALID_PORT_ATTACH  CELL_ERROR_CAST(0x80410502)  /* too many Glue ports */
#define CELL_DAISY_ERROR_NOT_IMPLEMENTED      CELL_ERROR_CAST(0x80410503)
#define CELL_DAISY_ERROR_STAT                 CELL_ERROR_CAST(0x8041050f)  /* target disabled */
#define CELL_DAISY_ERROR_AGAIN                CELL_ERROR_CAST(0x80410511)  /* pending list full */
#define CELL_DAISY_ERROR_INVAL                CELL_ERROR_CAST(0x80410512)  /* ends disagree on tSize / type size */
#define CELL_DAISY_ERROR_BUSY                 CELL_ERROR_CAST(0x8041051A)  /* the PPU end is still initialising */

#endif /* PS3TC_CELL_DAISY_ERROR_H */
