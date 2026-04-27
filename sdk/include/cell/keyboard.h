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

/* ---- type aliases ---------------------------------------------- */
typedef KbMkey      CellKbMkey;
typedef KbLed       CellKbLed;
typedef KbConfig    CellKbConfig;
typedef KbRmode     CellKbReadMode;
typedef KbCodeType  CellKbCodeType;
typedef KbMapping   CellKbMappingType;

/* CellKbData uses an anonymous union to expose both field names —
 * `len` (reference SDK) and `nb_keycode` (PSL1GHT) — at the same
 * offset.  Layout is byte-identical to PSL1GHT's KbData. */
typedef struct CellKbData {
    CellKbLed   led;
    CellKbMkey  mkey;
    union {
        int32_t len;
        int32_t nb_keycode;
    };
    uint16_t    keycode[MAX_KEYCODES];
} CellKbData;
_Static_assert(sizeof(CellKbData) == sizeof(KbData),
               "CellKbData / KbData layout drift");

/* CellKbInfo: same union trick to expose Sony names (max_connect /
 * now_connect) alongside PSL1GHT's (max / connected).  Status array
 * sized to MAX_KEYBOARDS to match PSL1GHT's KbInfo layout exactly. */
typedef struct CellKbInfo {
    union { uint32_t max;       uint32_t max_connect; };
    union { uint32_t connected; uint32_t now_connect; };
    uint32_t info;
    uint8_t  status[MAX_KEYBOARDS];
} CellKbInfo;
_Static_assert(sizeof(CellKbInfo) == sizeof(KbInfo),
               "CellKbInfo / KbInfo layout drift");

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
