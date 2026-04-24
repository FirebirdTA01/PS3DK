/*
 * PS3 Custom Toolchain - libio legacy-name compatibility wrappers.
 *
 * Provides the PSL1GHT-flavoured ioPad / ioKb / ioMouse names as thin
 * shims over the cell-SDK cellPad / cellKb / cellMouse NID stubs in our
 * nidgen-generated libio_stub archive.  The combined object (nidgen
 * stubs plus these wrappers) is installed as $PS3DK/ppu/lib/libio.a,
 * which shadows PSL1GHT's libio.a at link time so samples pull zero
 * bytes from the PSL1GHT install tree for the pad, kb, and mouse surface.
 *
 * PSL1GHT's padInfo, padInfo2, padData and friends are binary-compatible
 * with our CellPadInfo / CellPadData types (verified layout); likewise
 * for kb and mouse.  We cast the struct pointers at the wrapper boundary.
 */

#include <stdint.h>
#include <io/pad.h>
#include <io/kb.h>
#include <io/mouse.h>
#include <ppu-types.h>

/* ====================================================================
 * cellPad* / cellKb* / cellMouse* extern prototypes — provided by the
 * nidgen stub archive's sceStub.text / .opd.
 * ==================================================================== */

/* Pad */
extern int32_t cellPadInit(uint32_t max_connect);
extern int32_t cellPadEnd(void);
extern int32_t cellPadClearBuf(uint32_t port_no);
extern int32_t cellPadGetInfo(void *info);
extern int32_t cellPadGetInfo2(void *info);
extern int32_t cellPadGetData(uint32_t port_no, void *data);
extern int32_t cellPadGetDataExtra(uint32_t port_no, uint32_t *device_type, void *data);
extern int32_t cellPadGetRawData(uint32_t port_no, void *data);
extern int32_t cellPadGetCapabilityInfo(uint32_t port_no, void *info);
extern int32_t cellPadSetPortSetting(uint32_t port_no, uint32_t setting);
extern int32_t cellPadSetPressMode(uint32_t port_no, uint32_t mode);
extern int32_t cellPadInfoPressMode(uint32_t port_no);
extern int32_t cellPadSetSensorMode(uint32_t port_no, uint32_t mode);
extern int32_t cellPadInfoSensorMode(uint32_t port_no);
extern int32_t cellPadSetActDirect(uint32_t port_no, void *param);
extern int32_t cellPadLddRegisterController(void);
extern int32_t cellPadLddUnregisterController(int32_t handle);
extern int32_t cellPadLddDataInsert(int32_t handle, void *data);
extern int32_t cellPadLddGetPortNo(int32_t handle);
extern int32_t cellPadPeriphGetInfo(void *info);
extern int32_t cellPadPeriphGetData(uint32_t port_no, void *data);

/* Kb */
extern int32_t cellKbInit(uint32_t max_connect);
extern int32_t cellKbEnd(void);
extern int32_t cellKbClearBuf(uint32_t kb_no);
extern int32_t cellKbRead(uint32_t kb_no, void *data);
extern int32_t cellKbSetReadMode(uint32_t kb_no, uint32_t rmode);
extern int32_t cellKbSetCodeType(uint32_t kb_no, uint32_t ctype);
extern uint16_t cellKbCnvRawCode(uint32_t mapping, uint32_t mkey, uint32_t led, uint16_t rawcode);
extern int32_t cellKbSetLEDStatus(uint32_t kb_no, uint8_t led);
extern int32_t cellKbGetInfo(void *info);
extern int32_t cellKbGetConfiguration(uint32_t kb_no, void *config);

/* Mouse */
extern int32_t cellMouseInit(uint32_t max_connect);
extern int32_t cellMouseEnd(void);
extern int32_t cellMouseClearBuf(uint32_t port_no);
extern int32_t cellMouseGetInfo(void *info);
extern int32_t cellMouseGetData(uint32_t port_no, void *data);
extern int32_t cellMouseGetDataList(uint32_t port_no, void *list);
extern int32_t cellMouseGetRawData(uint32_t port_no, void *raw);
extern int32_t cellMouseGetTabletDataList(uint32_t port_no, void *list);
extern int32_t cellMouseInfoTabletMode(uint32_t port_no, void *info);
extern int32_t cellMouseSetTabletMode(uint32_t port_no, uint32_t mode);

/* ====================================================================
 * Legacy ioPad* wrappers
 * ==================================================================== */

s32 ioPadInit(u32 max)                    { return (s32)cellPadInit(max); }
s32 ioPadEnd(void)                        { return (s32)cellPadEnd(); }
s32 ioPadClearBuf(u32 port)               { return (s32)cellPadClearBuf(port); }
s32 ioPadGetInfo(padInfo *info)           { return (s32)cellPadGetInfo(info); }
s32 ioPadGetInfo2(padInfo2 *info)         { return (s32)cellPadGetInfo2(info); }
s32 ioPadGetData(u32 port, padData *data) { return (s32)cellPadGetData(port, data); }
s32 ioPadGetDataExtra(u32 port, u32 *type, padData *data)
                                          { return (s32)cellPadGetDataExtra(port, type, data); }
s32 ioPadGetRawData(u32 port, padData *data)
                                          { return (s32)cellPadGetRawData(port, data); }
s32 ioPadGetCapabilityInfo(u32 port, padCapabilityInfo *caps)
                                          { return (s32)cellPadGetCapabilityInfo(port, caps); }
s32 ioPadSetPortSetting(u32 port, u32 s)  { return (s32)cellPadSetPortSetting(port, s); }
s32 ioPadSetPressMode(u32 port, u32 m)    { return (s32)cellPadSetPressMode(port, m); }
s32 ioPadInfoPressMode(u32 port)          { return (s32)cellPadInfoPressMode(port); }
s32 ioPadSetSensorMode(u32 port, u32 m)   { return (s32)cellPadSetSensorMode(port, m); }
s32 ioPadInfoSensorMode(u32 port)         { return (s32)cellPadInfoSensorMode(port); }
u32 ioPadSetActDirect(u32 port, padActParam *p)
                                          { return (u32)cellPadSetActDirect(port, p); }
s32 ioPadLddRegisterController(void)      { return (s32)cellPadLddRegisterController(); }
s32 ioPadLddUnregisterController(s32 h)   { return (s32)cellPadLddUnregisterController(h); }
u32 ioPadLddDataInsert(s32 h, padData *d) { return (u32)cellPadLddDataInsert(h, d); }
s32 ioPadLddGetPortNo(s32 h)              { return (s32)cellPadLddGetPortNo(h); }
s32 ioPadPeriphGetInfo(padPeriphInfo *i)  { return (s32)cellPadPeriphGetInfo(i); }
s32 ioPadPeriphGetData(u32 port, padPeriphData *d)
                                          { return (s32)cellPadPeriphGetData(port, d); }

/* ====================================================================
 * Legacy ioKb* wrappers
 * ==================================================================== */

s32 ioKbInit(const u32 max)                         { return (s32)cellKbInit(max); }
s32 ioKbEnd(void)                                   { return (s32)cellKbEnd(); }
s32 ioKbClearBuf(const u32 kb_no)                   { return (s32)cellKbClearBuf(kb_no); }
s32 ioKbRead(const u32 kb_no, KbData *data)         { return (s32)cellKbRead(kb_no, data); }
s32 ioKbSetReadMode(const u32 kb_no, const KbRmode rmode)
                                                    { return (s32)cellKbSetReadMode(kb_no, (uint32_t)rmode); }
s32 ioKbSetCodeType(const u32 kb_no, const KbCodeType ctype)
                                                    { return (s32)cellKbSetCodeType(kb_no, (uint32_t)ctype); }
u16 ioKbCnvRawCode(const KbMapping mapping, const KbMkey mkey, const KbLed led, const u16 rawcode)
{
    /* KbMkey and KbLed are struct-wrapped unions; the sys_io SPRX expects
     * the raw 32-bit bitmask at the NID boundary.  PSL1GHT exposes the u32
     * under the nested union member _KbMkeyU.mkeys / _KbLedU.leds. */
    return cellKbCnvRawCode((uint32_t)mapping, mkey._KbMkeyU.mkeys,
                            led._KbLedU.leds, rawcode);
}
s32 ioKbSetLEDStatus(const u32 kb_no, const KbLed led)
                                                    { return (s32)cellKbSetLEDStatus(kb_no, led._KbLedU.leds); }
s32 ioKbGetInfo(KbInfo *info)                       { return (s32)cellKbGetInfo(info); }
s32 ioKbGetConfiguration(const u32 kb_no, KbConfig *config)
                                                    { return (s32)cellKbGetConfiguration(kb_no, config); }

/* ====================================================================
 * Legacy ioMouse* wrappers
 * ==================================================================== */

s32 ioMouseInit(u32 max)                         { return (s32)cellMouseInit(max); }
s32 ioMouseEnd(void)                             { return (s32)cellMouseEnd(); }
s32 ioMouseClearBuf(u32 port)                    { return (s32)cellMouseClearBuf(port); }
s32 ioMouseGetInfo(mouseInfo *info)              { return (s32)cellMouseGetInfo(info); }
s32 ioMouseGetRawData(u32 port, mouseRawData *r) { return (s32)cellMouseGetRawData(port, r); }
s32 ioMouseGetData(u32 port, mouseData *d)       { return (s32)cellMouseGetData(port, d); }
s32 ioMouseGetDataList(u32 port, mouseDataList *l)
                                                 { return (s32)cellMouseGetDataList(port, l); }
s32 ioMouseGetTabletDataList(u32 port, mouseTabletDataList *l)
                                                 { return (s32)cellMouseGetTabletDataList(port, l); }
s32 ioMouseInfoTabletMode(u32 port, mouseInfoTablet *i)
                                                 { return (s32)cellMouseInfoTabletMode(port, i); }
s32 ioMouseSetTabletMode(u32 port, u32 mode)     { return (s32)cellMouseSetTabletMode(port, mode); }
