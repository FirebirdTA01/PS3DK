/*! \file cell/mouse.h
 \brief Reference-SDK source-compat surface for cellMouse input.

  Provides cellMouse* function decls + Cell-named typedefs/constants
  on top of PSL1GHT's <io/mouse.h>.  Function symbols resolve through
  libio.a; types/constants are aliases over PSL1GHT's MouseData /
  MouseInfo layouts.
*/

#ifndef PS3TC_CELL_MOUSE_H
#define PS3TC_CELL_MOUSE_H

#include <stdint.h>
#include <io/mouse.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- type aliases ---------------------------------------------- */
typedef mouseData         CellMouseData;
typedef mouseDataList     CellMouseList;
typedef mouseInfo         CellMouseInfo;
typedef mouseRawData      CellMouseRawData;
typedef mouseTabletData   CellMouseTabletData;

/* ---- constants -------------------------------------------------- */
#define CELL_MOUSE_MAX_CODES                MOUSE_MAX_CODES
#define CELL_MOUSE_MAX_DATA_LIST            MOUSE_MAX_DATA_LIST
#define CELL_MOUSE_DATA_UPDATE              0x01
#define CELL_MOUSE_DATA_NON                 0x00
#define CELL_MOUSE_STATUS_CONNECTED         0x01
#define CELL_MOUSE_STATUS_DISCONNECTED      0x00

/* ---- entry points ---------------------------------------------- */
extern int32_t cellMouseInit(uint32_t max_connect);
extern int32_t cellMouseEnd(void);
extern int32_t cellMouseClearBuf(uint32_t port_no);
extern int32_t cellMouseGetInfo(CellMouseInfo *info);
extern int32_t cellMouseGetData(uint32_t port_no, CellMouseData *data);
extern int32_t cellMouseGetDataList(uint32_t port_no, CellMouseList *list);
extern int32_t cellMouseGetRawData(uint32_t port_no, CellMouseRawData *data);
extern int32_t cellMouseGetTabletDataList(uint32_t port_no, void *list);
extern int32_t cellMouseSetTabletMode(uint32_t port_no, uint32_t mode);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_MOUSE_H */
