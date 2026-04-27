/*! \file cell/keyboard.h
 \brief Reference-SDK source-compat surface for cellKb keyboard input.

  Provides cellKb* function decls + Cell-named typedefs/constants on
  top of PSL1GHT's <io/kb.h>.  Function symbols resolve through
  libio.a (nidgen-emitted Sony stubs); types/constants are local
  aliases over PSL1GHT's KbData / KbInfo / KB_* layouts.
*/

#ifndef PS3TC_CELL_KEYBOARD_H
#define PS3TC_CELL_KEYBOARD_H

#include <stdint.h>
#include <io/kb.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- type aliases (byte-identical layouts) ---------------------- */
typedef KbData      CellKbData;
typedef KbInfo      CellKbInfo;
typedef KbMkey      CellKbMkey;
typedef KbLed       CellKbLed;
typedef KbConfig    CellKbConfig;
typedef KbRmode     CellKbReadMode;
typedef KbCodeType  CellKbCodeType;
typedef KbMapping   CellKbMappingType;

/* ---- constants -------------------------------------------------- */
#define CELL_KB_RAWDAT                  KB_RAWDAT
#define CELL_KB_KEYPAD                  KB_KEYPAD
#define CELL_KB_CODETYPE_RAW            KB_CODETYPE_RAW
#define CELL_KB_CODETYPE_ASCII          KB_CODETYPE_ASCII
#define CELL_KB_RMODE_INPUTCHAR         KB_RMODE_INPUTCHAR
#define CELL_KB_RMODE_PACKET            KB_RMODE_PACKET
#define CELL_KB_MAX_KEYBOARDS           MAX_KB_PORT_NUM
#define CELL_KB_MAX_KEYCODES            MAX_KEYCODES

/* ---- entry points (resolved via libio.a) ----------------------- */
extern int32_t cellKbInit(uint32_t max_connect);
extern int32_t cellKbEnd(void);
extern int32_t cellKbClearBuf(uint32_t port_no);
extern int32_t cellKbGetInfo(CellKbInfo *info);
extern int32_t cellKbRead(uint32_t port_no, CellKbData *data);
extern int32_t cellKbSetCodeType(uint32_t port_no, uint32_t type);
extern int32_t cellKbSetLEDStatus(uint32_t port_no, uint8_t led);
extern int32_t cellKbSetReadMode(uint32_t port_no, uint32_t rmode);
extern int32_t cellKbGetConfiguration(uint32_t port_no, CellKbConfig *config);
extern uint16_t cellKbCnvRawCode(uint32_t arrange, uint32_t mkey, uint32_t led, uint16_t rawcode);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_KEYBOARD_H */
