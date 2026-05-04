# libusbd (cellUsbd) — USB device driver framework

| Property | Value |
|---|---|
| Header umbrella | `<cell/usbd.h>` |
| Stub archive | `libusbd_stub.a` (nidgen + legacy wrappers) |
| Legacy alias | `libusb.a` → `libusbd_stub.a` (symlink) |
| Entry points | 35 cellUsbd* (24 documented + 11 firmware-only) + 35 usb* legacy forwarders |
| Reference headers | 4 files (usbd.h umbrella, usb.h USB-IF types, libusbd.h API, error.h) |

## Surface

libusbd provides a USB device driver (LDD) framework for the PS3. It exposes:

| Area | Description |
|---|---|
| **Init/End** | `cellUsbdInit`, `cellUsbdEnd` — bootstrap |
| **LDD management** | Register/unregister Logical Device Driver ops tables, with optional vendor/product filtering |
| **Pipe I/O** | Open/close pipes, control/bulk/interrupt/isochronous transfers with completion callbacks |
| **Device info** | Get device location, speed, scan descriptors, private data |
| **Memory** | Allocate/free memory for USB transfers |
| **Thread priority** | Get/set priorities for USB event/callback threads |

### Function table (24 documented)

| Function | Signature |
|---|---|
| `cellUsbdInit` | `int32_t(void)` |
| `cellUsbdEnd` | `int32_t(void)` |
| `cellUsbdRegisterLdd` | `int32_t(CellUsbdLddOps*)` |
| `cellUsbdUnregisterLdd` | `int32_t(CellUsbdLddOps*)` |
| `cellUsbdRegisterExtraLdd` | `int32_t(CellUsbdLddOps*, uint16_t idVendor, uint16_t idProduct)` |
| `cellUsbdRegisterExtraLdd2` | `int32_t(CellUsbdLddOps*, uint16_t idVendor, uint16_t idProductMin, uint16_t idProductMax)` |
| `cellUsbdUnregisterExtraLdd` | `int32_t(CellUsbdLddOps*)` |
| `cellUsbdOpenPipe` | `int32_t(int32_t dev_id, UsbEndpointDescriptor*)` |
| `cellUsbdClosePipe` | `int32_t(int32_t pipe_id)` |
| `cellUsbdScanStaticDescriptor` | `void*(int32_t dev_id, void *ptr, unsigned char type)` |
| `cellUsbdSetPrivateData` | `int32_t(int32_t dev_id, void *priv)` |
| `cellUsbdGetPrivateData` | `void*(int32_t dev_id)` |
| `cellUsbdGetDeviceLocation` | `int32_t(int32_t dev_id, unsigned char *location)` |
| `cellUsbdGetDeviceSpeed` | `int32_t(int32_t dev_id, uint8_t *speed)` |
| `cellUsbdControlTransfer` | `int32_t(int32_t pipe_id, UsbDeviceRequest*, void *buf, CellUsbdDoneCallback, void *arg)` |
| `cellUsbdBulkTransfer` | `int32_t(int32_t pipe_id, void *buf, int32_t len, CellUsbdDoneCallback, void *arg)` |
| `cellUsbdInterruptTransfer` | `int32_t(int32_t pipe_id, void *buf, int32_t len, CellUsbdDoneCallback, void *arg)` |
| `cellUsbdIsochronousTransfer` | `int32_t(int32_t pipe_id, CellUsbdIsochRequest*, CellUsbdIsochDoneCallback, void *arg)` |
| `cellUsbdHSIsochronousTransfer` | `int32_t(int32_t pipe_id, CellUsbdHSIsochRequest*, CellUsbdHSIsochDoneCallback, void *arg)` |
| `cellUsbdSetThreadPriority2` | `int32_t(int32_t event_prio, int32_t usbd_prio, int32_t callback_prio)` |
| `cellUsbdGetThreadPriority` | `int32_t(int32_t thread_type)` |
| `cellUsbdSetThreadPriority` | `int32_t(int32_t thread_type)` |
| `cellUsbdAllocateMemory` | `int32_t(void**, size_t)` |
| `cellUsbdFreeMemory` | `int32_t(void*)` |

Plus 22 convenience macros (`cellUsbdGetDescriptor`, `cellUsbdSetAddress`, `cellUsbdSetConfiguration`, etc.) that expand a `UsbDeviceRequest` construction followed by `cellUsbdControlTransfer`. These use GNU statement expressions (`({ ... })`) and require `-std=gnu11` / `-std=gnu++17`.

### Firmware-only NIDs (11)

`cellUsbdUnknown1` through `cellUsbdUnknown11` — extracted from the SPRX by PSL1GHT NID mining. Not declared in our headers (undocumented semantics), but present as nidgen stubs so PSL1GHT-era callers can link.

### Legacy PSL1GHT surface (35 usb* names)

`sdk/libusb_legacy/` provides thin forwarders mapping PSL1GHT's short `usb*` names to the canonical `cellUsbd*` stubs:

| Pattern | Count | Example |
|---|---|---|
| Straight-cell | 14 | `usbInit` → `cellUsbdInit` |
| Ex-renames | 10 | `usbBulkTransferEx` → `cellUsbdBulkTransfer` |
| Unknown NIDs | 11 | `usbUnknown1` → `cellUsbdUnknown1` |

The `libusb.a` symlink → `libusbd_stub.a` preserves backward compatibility for PSL1GHT Makefiles.

## Error codes

All errors encode in `0x8011xxxx` (facility `CELL_ERROR_FACILITY_USB` = 0x011):

| Error | Value |
|---|---|
| `CELL_USBD_ERROR_NOT_INITIALIZED` | 0x80110001 |
| `CELL_USBD_ERROR_ALREADY_INITIALIZED` | 0x80110002 |
| `CELL_USBD_ERROR_NO_MEMORY` | 0x80110003 |
| `CELL_USBD_ERROR_INVALID_PARAM` | 0x80110004 |
| `CELL_USBD_ERROR_INVALID_TRANSFER_TYPE` | 0x80110005 |
| `CELL_USBD_ERROR_LDD_ALREADY_REGISTERED` | 0x80110006 |
| `CELL_USBD_ERROR_LDD_NOT_ALLOCATED` | 0x80110007 |
| `CELL_USBD_ERROR_LDD_NOT_RELEASED` | 0x80110008 |
| `CELL_USBD_ERROR_LDD_NOT_FOUND` | 0x80110009 |
| `CELL_USBD_ERROR_DEVICE_NOT_FOUND` | 0x8011000a |
| `CELL_USBD_ERROR_PIPE_NOT_ALLOCATED` | 0x8011000b |
| `CELL_USBD_ERROR_PIPE_NOT_RELEASED` | 0x8011000c |
| `CELL_USBD_ERROR_PIPE_NOT_FOUND` | 0x8011000d |
| `CELL_USBD_ERROR_IOREQ_NOT_ALLOCATED` | 0x8011000e |
| `CELL_USBD_ERROR_IOREQ_NOT_RELEASED` | 0x8011000f |
| `CELL_USBD_ERROR_IOREQ_NOT_FOUND` | 0x80110010 |
| `CELL_USBD_ERROR_CANNOT_GET_DESCRIPTOR` | 0x80110011 |
| `CELL_USBD_ERROR_FATAL` | 0x801100ff |

LDD callback return codes use plain `int32_t` values (not `CELL_ERROR_CAST`): `CELL_USBD_PROBE_SUCCEEDED` = 0, `CELL_USBD_PROBE_FAILED` = -1, etc.

## Initialisation lifecycle

1. `cellUsbdInit()` — initialises the USB device driver framework.
2. Register LDD ops: `cellUsbdRegisterLdd` (or `RegisterExtraLdd` with vendor/product filtering).
3. When a device matches, the LDD's `probe` callback fires. On success, `attach` is called.
4. Pipe I/O: `cellUsbdOpenPipe` → `cellUsbdControlTransfer` / `BulkTransfer` / etc. with a `CellUsbdDoneCallback` completion handler.
5. Tear down: `cellUsbdClosePipe`, `cellUsbdUnregisterLdd`, `cellUsbdEnd`.

## Notable ABI quirks

### ATTRIBUTE_PRXPTR

| Struct | PRXPTR fields |
|---|---|
| `CellUsbdLddOps` | `name`, `probe`, `attach`, `detach` — all four pointer members |
| `CellUsbdHSIsochRequest` | `buffer_base` |
| `CellUsbdIsochRequest` | `buffer_base` |
| `UsbDeviceRequest` | None — all scalar fields |

### Isochronous PSW bitfields

`CellUsbdIsochPswLen` packs `len:11, reserved:1, PSW:4` into a `uint16_t`. `CellUsbdHSIsochPswLen` packs `len:12, PSW:4` (no reserved bit). These bitfield layouts are GCC-specific and must match the SPRX's interpretation exactly.

### GNU statement expressions

All 22 `cellUsbd*` convenience macros use `({ UsbDeviceRequest _dr; ... _dr.wValue = ...; cellUsbdControlTransfer(...); })` syntax. These compile only in GNU mode (`-std=gnu11` / `-std=gnu++17`), not strict ISO C.

### Symlink direction

`libusb.a` → `libusbd_stub.a` (inverted from PSL1GHT's old `libusbd_stub.a → libusb.a`). The nidgen-generated `libusbd_stub.a` is the canonical archive; `libusb.a` is a backward-compatibility alias.

### USB descriptor alignment

`UsbDeviceRequest` and USB standard descriptors are naturally aligned (no `__attribute__((packed))`). `UsbConfigurationDescriptor` has natural alignment (sizeof = 10), which differs from the USB wire-format size of 9 — the single byte of tail padding is ABI-correct for the SPRX.

## Sample

`samples/lv2/hello-ppu-usbd/` — PPU smoke test validated in RPCS3: loads `CELL_SYSMODULE_USBD`, then Init → AllocateMemory → FreeMemory → SetThreadPriority2 → End.
