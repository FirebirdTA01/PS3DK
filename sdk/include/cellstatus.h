/* cellstatus.h -- CELL_OK and status-code classification macros.
 *
 * Pure macro header with no runtime dependencies.  CELL_OK is the
 * canonical success code (0); CELL_STATUS_IS_FAILURE / _IS_SUCCESS
 * test the top bit convention used across the reference SDK family.
 */
#ifndef PS3DK_CELLSTATUS_H
#define PS3DK_CELLSTATUS_H

#define CELL_OK 0

#ifndef SUCCEEDED
#define SUCCEEDED CELL_OK
#endif

#define CELL_STATUS_IS_FAILURE(status) ((status) & 0x80000000)
#define CELL_STATUS_IS_SUCCESS(status) (!((status) & 0x80000000))

#endif /* PS3DK_CELLSTATUS_H */
