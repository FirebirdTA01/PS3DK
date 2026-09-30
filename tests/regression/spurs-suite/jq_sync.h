/* spurs-jq-sync row: values shared by the PPU and the SPU job. */
#ifndef SUITE_JQ_SYNC_H
#define SUITE_JQ_SYNC_H

#define JS_ROWS      8
#define JS_COLS      8
#define JS_ROW       0u            /* job kinds (userData[2]) */
#define JS_COL       1u
#define JS_ROW_DONE  0x52000000u   /* a finished row writes JS_ROW_DONE | index */
#define JS_COL_DONE  0x43000000u   /* a finished column writes JS_COL_DONE | index */
#define JS_SPIN      4000000u      /* a row works this long before it writes */

#endif
