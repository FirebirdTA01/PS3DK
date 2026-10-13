# sysutil query libraries — the small-cell-surface stub archives

| Property | Value |
|---|---|
| Headers | the twelve `cell/sysutil_*.h` query headers below; each has a matching `sysutil/sysutil_*.h` forwarder |
| Stub archives | one each, `libsysutil_<name>_stub.a` |
| Availability | PPU static stub archives over system modules; link one per library against the surface it exposes. |

Twelve of the small-cell-surface sysutil libraries now have their own
headers and stub archives. Each one below is a PPU-side static archive
named `libsysutil_<name>_stub.a`, paired with a `cell/sysutil_<name>.h`
header and its matching `sysutil/sysutil_<name>.h` forwarder. The sample
at the bottom of this page exercises four of them end-to-end.

## Per-library table

| Name | Header (cell) | Stub archive | `CELL_SYSMODULE_*` (as declared in `<cell/sysmodule.h>`) | Entry points |
|---|---|---|---|---|
| photo_decode  | `<cell/sysutil_photo_decode.h>`   | `libsysutil_photo_decode_stub.a`   | `CELL_SYSMODULE_PHOTO_DECODE`   0xf02e | 4 |
| photo_export  | `<cell/sysutil_photo_export.h>`   | `libsysutil_photo_export_stub.a`   | `CELL_SYSMODULE_PHOTO_EXPORT`   0xf029 | 6 |
| photo_import  | `<cell/sysutil_photo_import.h>`   | `libsysutil_photo_import_stub.a`   | `CELL_SYSMODULE_PHOTO_IMPORT`   0xf02b | 2 |
| video_export  | `<cell/sysutil_video_export.h>`   | `libsysutil_video_export_stub.a`   | `CELL_SYSMODULE_VIDEO_EXPORT`   0x0036 | 6 |
| video_upload  | `<cell/sysutil_video_upload.h>`   | `libsysutil_video_upload_stub.a`   | `CELL_SYSMODULE_SYSUTIL_VIDEO_UPLOAD` 0x0041 | 1 |
| print         | `<cell/sysutil_print.h>`          | `libsysutil_print_stub.a`          | `CELL_SYSMODULE_PRINT`          0xf02a | 12 |
| rec           | `<cell/sysutil_rec.h>`            | `libsysutil_rec_stub.a`            | `CELL_SYSMODULE_SYSUTIL_REC`    0x0035 | 7 |
| remoteplay    | `<cell/sysutil_remoteplay.h>`     | `libsysutil_remoteplay_stub.a`     | (no dedicated constant in `<cell/sysmodule.h>`) | 4 |
| game_exec     | `<cell/sysutil_game_exec.h>`      | `libsysutil_game_exec_stub.a`      | `CELL_SYSMODULE_SYSUTIL_GAME_EXEC` 0x0037 | 5 |
| licensearea   | `<cell/sysutil_licensearea.h>`    | `libsysutil_licensearea_stub.a`    | `CELL_SYSMODULE_SYSUTIL_LICENSEAREA` 0x0049 | 1 |
| cross_controller | `<cell/sysutil_cross_controller.h>` | `libsysutil_cross_controller_stub.a` | `CELL_SYSMODULE_SYSUTIL_CROSS_CONTROLLER` 0x005c | 1 |
| sysconf_ext   | `<cell/sysutil_sysconf_ext.h>`    | `libsysutil_sysconf_ext_stub.a`    | `CELL_SYSMODULE_SYSUTIL_SYSCONF_EXT` 0x0042 | 1 |

## Surface

Each of the twelve declares only its own entry points — the per-library
counts in the column on the right. Representative per-call signatures, all
`int` unless noted:

| Library | Signature (one line per call) |
|---|---|
| licensearea  | `int cellSysutilGetLicenseArea(void)` |
| remoteplay   | `int cellRemotePlayGetStatus(void)`; `int cellRemotePlayGetComparativeVolume(float*)`; `int cellRemotePlaySetComparativeVolume(float)`; `int cellRemotePlayBreak(void)` |
| game_exec    | `int cellGameGetBootGameInfo(unsigned int*, char*, unsigned int*)`; `int cellGameSetExitParam(unsigned int)`; `int cellGameGetHomeLaunchOptionPath(char*, char*)`; `int cellGameGetHomeDataExportPath(char*)`; `int cellGameGetHomeDataImportPath(char*)` |
| sysconf_ext  | `int cellSysconfBtGetDeviceList(CellSysconfBtDeviceList*)` |
| photo_decode  | `int cellPhotoDecodeInitialize / Initialize2 / FromFile / Finalize(...)` |
| photo_export  | `int cellPhotoExportInitialize / Initialize2 / FromFile / FromFileWithCopy / Finalize / Progress(...)` |
| photo_import  | `int cellPhotoImport / cellPhotoImport2(...)` |
| video_export  | `int cellVideoExportInitialize / Initialize2 / FromFile / FromFileWithCopy / Finalize / Progress(...)` |
| video_upload  | `int32_t cellVideoUploadInitialize(const CellVideoUploadParam*, CellVideoUploadCallback, void*)` |
| print         | `int cellPrintLoadAsync / LoadAsync2 / UnloadAsync / OpenConfig / StartJob / EndJob / CancelJob / StartPage / EndPage / GetPrintableArea / SendBand / GetStatus(...)` |
| rec           | `size_t cellRecQueryMemSize(const CellRecParam*)`; `int cellRecOpen / cellRecStart / cellRecStop / cellRecClose / cellRecSetInfo / cellRecGetInfo(...)` |
| cross_controller | `int32_t cellCrossControllerInitialize(const CellCrossControllerParam*, const CellCrossControllerPackageInfo*, CellCrossControllerCallback, void*)` |

Full parameter lists are in the headers; the table above names the calls
in each library and the return types they return.

## Error codes

Each of the twelve defines its own error codes in its own header.
The `CELL_SYSUTIL_LICENSE_AREA_*` values (the result of
`cellSysutilGetLicenseArea`) are also in `<sysutil/sysutil_common.h>`:
`J 0`, `A 1`, `E 2`, `H 3`, `K 4`, `C 5`, `OTHER 100`.

## Notable ABI quirks

- **32-bit pointers.** photo_decode / photo_export / photo_import,
  video_export, video_upload, rec and cross_controller mark the pointer
  members of their parameter structs `ATTRIBUTE_PRXPTR` so they stay 32
  bits wide in LP64 programs, because the system module reads them as
  32-bit EAs.
- **System-written, fixed-layout structs.** A few structs are written back
  by the system module and carry an explicit fixed size in the header —
  `CellPhotoImportFileData` (1608 bytes), `CellVideoUploadParam` (48
  bytes) and `CellSysconfBtDeviceInfo` (96 bytes per device) in a
  `CellSysconfBtDeviceList` of 1536 bytes — so callers allocate to that exact size on both data models.
- **Callback-driven steps.** The photo / video export and import flows, the
  print `LoadAsync` steps, the rec open/start/stop/close sequence, the
  cross-controller transfer and the video upload report their results
  through the user's callback; each header's opening comment says what its
  callback receives.  `cellRecGetInfo` and `cellRecSetInfo` return their
  result directly.
- **rec version-dependent container size.** `CELL_REC_MAX_MEMORY_CONTAINER_SIZE`
  is `(9 * 1024 * 1024)` below SDK 3.0.0 and `(16 * 1024 * 1024)` at or
  above it, per the header's conditional on `CELL_SDK_VERSION`.

## Sample

`samples/sysutil/hello-ppu-sysutil-queries/` — loads the three sysmodules
it needs (`CELL_SYSMODULE_SYSUTIL_LICENSEAREA`, `CELL_SYSMODULE_SYSUTIL_GAME_EXEC`,
`CELL_SYSMODULE_SYSUTIL_SYSCONF_EXT`), queries the license area, the Remote
Play status and comparative audio volume, the boot-game info and the HOME
data-export path, and the paired Bluetooth device list; unloads the modules
and exits with PASS only when the license area is one of
`CELL_SYSUTIL_LICENSE_AREA_*`.
