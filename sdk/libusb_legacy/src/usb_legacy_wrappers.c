/* sdk/libusb_legacy/src/usb_legacy_wrappers.c
 *
 * PSL1GHT-compatible usb* → cellUsbd* thin forwarders.
 *
 * The cell SDK ABI surface uses cellUsbd* names; PSL1GHT homebrew
 * uses the shorter usb* names with some Ex-renames.  Each wrapper
 * here is a one-line call into the corresponding nidgen-generated
 * cellUsbd* stub.  35 entry points total:
 *   14 straight-cell names (Init/End/AllocateMemory/...)
 *   10 Ex-renames (usb*Ex mapped back to cellUsbd*Ex-less names)
 *   11 Unknown firmware-only NIDs
 */

#include <stdint.h>
#include <stddef.h>

/* ---- externs (cellUsbd* stubs, resolved by libusbd_stub.a) ------ */

extern int32_t cellUsbdInit(void);
extern int32_t cellUsbdEnd(void);
extern int32_t cellUsbdAllocateMemory(void **ptr, size_t size);
extern int32_t cellUsbdFreeMemory(void *ptr);
extern int32_t cellUsbdOpenPipe(int32_t dev_id, void *ed);
extern int32_t cellUsbdClosePipe(int32_t pipe_id);
extern int32_t cellUsbdGetDeviceLocation(int32_t dev_id, unsigned char *location);
extern int32_t cellUsbdGetDeviceSpeed(int32_t dev_id, uint8_t *speed);
extern int32_t cellUsbdGetPrivateData(int32_t dev_id);
extern int32_t cellUsbdSetPrivateData(int32_t dev_id, void *priv);
extern int32_t cellUsbdGetThreadPriority(int32_t thread_type);
extern int32_t cellUsbdSetThreadPriority(int32_t thread_type);
extern int32_t cellUsbdSetThreadPriority2(int32_t event_prio, int32_t usbd_prio, int32_t callback_prio);
extern void *cellUsbdScanStaticDescriptor(int32_t dev_id, void *ptr, unsigned char type);

extern int32_t cellUsbdControlTransfer(int32_t pipe_id, void *dr, void *buf,
                                       void *done_cb, void *arg);
extern int32_t cellUsbdBulkTransfer(int32_t pipe_id, void *buf, int32_t len,
                                     void *done_cb, void *arg);
extern int32_t cellUsbdInterruptTransfer(int32_t pipe_id, void *buf, int32_t len,
                                           void *done_cb, void *arg);
extern int32_t cellUsbdIsochronousTransfer(int32_t pipe_id, void *req,
                                            void *done_cb, void *arg);
extern int32_t cellUsbdHSIsochronousTransfer(int32_t pipe_id, void *req,
                                               void *done_cb, void *arg);
extern int32_t cellUsbdRegisterLdd(void *lddops);
extern int32_t cellUsbdUnregisterLdd(void *lddops);
extern int32_t cellUsbdRegisterExtraLdd(void *lddops, uint16_t id_vendor, uint16_t id_product);
extern int32_t cellUsbdRegisterExtraLdd2(void *lddops, uint16_t id_vendor,
                                          uint16_t id_product_min, uint16_t id_product_max);
extern int32_t cellUsbdUnregisterExtraLdd(void *lddops);

extern int32_t cellUsbdUnknown1(void);
extern int32_t cellUsbdUnknown2(void);
extern int32_t cellUsbdUnknown3(void);
extern int32_t cellUsbdUnknown4(void);
extern int32_t cellUsbdUnknown5(void);
extern int32_t cellUsbdUnknown6(void);
extern int32_t cellUsbdUnknown7(void);
extern int32_t cellUsbdUnknown8(void);
extern int32_t cellUsbdUnknown9(void);
extern int32_t cellUsbdUnknown10(void);
extern int32_t cellUsbdUnknown11(void);

/* ---- 14 straight-cell names ------------------------------------ */

int32_t usbInit(void)                        { return cellUsbdInit(); }
int32_t usbEnd(void)                         { return cellUsbdEnd(); }
int32_t usbAllocateMemory(void **ptr, size_t size) { return cellUsbdAllocateMemory(ptr, size); }
int32_t usbFreeMemory(void *ptr)             { return cellUsbdFreeMemory(ptr); }
int32_t usbClosePipe(int32_t pipe_id)        { return cellUsbdClosePipe(pipe_id); }
int32_t usbOpenPipe(int32_t dev_id, void *ed){ return cellUsbdOpenPipe(dev_id, ed); }
int32_t usbGetDeviceLocation(int32_t dev_id, unsigned char *location)
    { return cellUsbdGetDeviceLocation(dev_id, location); }
int32_t usbGetDeviceSpeed(int32_t dev_id, uint8_t *speed)
    { return cellUsbdGetDeviceSpeed(dev_id, speed); }
void *usbGetPrivateData(int32_t dev_id)      { return (void *)(uintptr_t)cellUsbdGetPrivateData(dev_id); }
int32_t usbSetPrivateData(int32_t dev_id, void *priv)
    { return cellUsbdSetPrivateData(dev_id, priv); }
int32_t usbGetThreadPriority(int32_t thread_type)
    { return cellUsbdGetThreadPriority(thread_type); }
int32_t usbSetThreadPriority(int32_t prio)
    { return cellUsbdSetThreadPriority(prio); }
int32_t usbSetThreadPriority2(int32_t event_prio, int32_t usbd_prio, int32_t callback_prio)
    { return cellUsbdSetThreadPriority2(event_prio, usbd_prio, callback_prio); }
void *usbScanStaticDescriptor(int32_t dev_id, void *ptr, unsigned char type)
    { return cellUsbdScanStaticDescriptor(dev_id, ptr, type); }

/* ---- 10 Ex-renames (usb*Ex → cellUsbd*Xxx, no Ex suffix) ------- */

int32_t usbBulkTransferEx(int32_t pipe_id, void *buf, int32_t len,
                          void *done_cb, void *arg)
    { return cellUsbdBulkTransfer(pipe_id, buf, len, done_cb, arg); }

int32_t usbControlTransferEx(int32_t pipe_id, void *dr, void *buf,
                             void *done_cb, void *arg)
    { return cellUsbdControlTransfer(pipe_id, dr, buf, done_cb, arg); }

int32_t usbInterruptTransferEx(int32_t pipe_id, void *buf, int32_t len,
                               void *done_cb, void *arg)
    { return cellUsbdInterruptTransfer(pipe_id, buf, len, done_cb, arg); }

int32_t usbIsochronousTransferEx(int32_t pipe_id, void *req,
                                 void *done_cb, void *arg)
    { return cellUsbdIsochronousTransfer(pipe_id, req, done_cb, arg); }

int32_t usbHSIsochronousTransferEx(int32_t pipe_id, void *req,
                                   void *done_cb, void *arg)
    { return cellUsbdHSIsochronousTransfer(pipe_id, req, done_cb, arg); }

int32_t usbRegisterLddEx(void *lddops)
    { return cellUsbdRegisterLdd(lddops); }

int32_t usbUnregisterLddEx(void *lddops)
    { return cellUsbdUnregisterLdd(lddops); }

int32_t usbRegisterExtraLddEx(void *lddops, uint16_t id_vendor, uint16_t id_product)
    { return cellUsbdRegisterExtraLdd(lddops, id_vendor, id_product); }

int32_t usbRegisterExtraLdd2Ex(void *lddops, uint16_t id_vendor,
                               uint16_t id_product_min, uint16_t id_product_max)
    { return cellUsbdRegisterExtraLdd2(lddops, id_vendor, id_product_min, id_product_max); }

int32_t usbUnregisterExtraLddEx(void *lddops)
    { return cellUsbdUnregisterExtraLdd(lddops); }

/* ---- 11 firmware-only Unknown NIDs ----------------------------- */

int32_t usbUnknown1(void)   { return cellUsbdUnknown1(); }
int32_t usbUnknown2(void)   { return cellUsbdUnknown2(); }
int32_t usbUnknown3(void)   { return cellUsbdUnknown3(); }
int32_t usbUnknown4(void)   { return cellUsbdUnknown4(); }
int32_t usbUnknown5(void)   { return cellUsbdUnknown5(); }
int32_t usbUnknown6(void)   { return cellUsbdUnknown6(); }
int32_t usbUnknown7(void)   { return cellUsbdUnknown7(); }
int32_t usbUnknown8(void)   { return cellUsbdUnknown8(); }
int32_t usbUnknown9(void)   { return cellUsbdUnknown9(); }
int32_t usbUnknown10(void)  { return cellUsbdUnknown10(); }
int32_t usbUnknown11(void)  { return cellUsbdUnknown11(); }
