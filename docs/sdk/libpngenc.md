# libpngenc (cellPngEnc) — PNG encoder

| Property | Value |
|---|---|
| Headers | `<cell/codec/pngenc.h>`, shared types in `<cell/codec/pngcom.h>` |
| Stub archive | `libpngenc_stub.a` (nidgen) |
| SPRX module | `cellPngEnc` |
| Entry points | 9 |
| Required sysmodules | `CELL_SYSMODULE_PNGENC` (0x0052) |

## Surface

cellPngEnc encodes raw RGBA/Grayscale pixel data to PNG bitstreams with configurable compression level, filter type, ancillary chunks, and stream/file output.

### Function table (9)

| Function | Signature |
|---|---|
| `cellPngEncQueryAttr` | `int32_t(const CellPngEncConfig*, CellPngEncAttr*)` |
| `cellPngEncOpen` | `int32_t(const CellPngEncConfig*, const CellPngEncResource*, CellPngEncHandle*)` |
| `cellPngEncOpenEx` | `int32_t(const CellPngEncConfig*, const CellPngEncResourceEx*, CellPngEncHandle*)` |
| `cellPngEncWaitForInput` | `int32_t(CellPngEncHandle, bool)` |
| `cellPngEncEncodePicture` | `int32_t(CellPngEncHandle, const CellPngEncPicture*, const CellPngEncEncodeParam*, const CellPngEncOutputParam*)` |
| `cellPngEncWaitForOutput` | `int32_t(CellPngEncHandle, uint32_t*, bool)` |
| `cellPngEncGetStreamInfo` | `int32_t(CellPngEncHandle, CellPngEncStreamInfo*)` |
| `cellPngEncReset` | `int32_t(CellPngEncHandle)` |
| `cellPngEncClose` | `int32_t(CellPngEncHandle)` |

## Error codes

| Error | Value |
|---|---|
| `CELL_PNGENC_ERROR_ARG` | 0x80611291 |
| `CELL_PNGENC_ERROR_SEQ` | 0x80611292 |
| `CELL_PNGENC_ERROR_FATAL` | 0x80611296 |
| `CELL_PNGENC_ERROR_STREAM_ABORT` | 0x806112A1 |
| `CELL_PNGENC_ERROR_STREAM_FILE_OPEN` | 0x806112A4 |

## Notable ABI quirks

### PRXPTR audit

| Struct | PRXPTR fields |
|---|---|
| `CellPngEncConfig` | `exParamList` |
| `CellPngEncResource`/`ResourceEx` | `memAddr`, `spurs` |
| `CellPngEncPicture` | `pictureAddr` |
| `CellPngEncAncillaryChunk` | `chunkData` |
| `CellPngEncOutputParam`/`StreamInfo` | `streamAddr` |

Width-sensitive: all `size_t` fields (memSize, limitSize, streamSize) converted to `uint32_t`. `CellPngEncHandle` = `uint32_t`.

### Shared PNG types

`<cell/codec/pngcom.h>` provides CellPngPLTE, CellPngTRNS, CellPngTExtInfo, etc. — shared with libpngdec.

## Sample

`samples/codec/hello-ppu-pngenc/` — PPU smoke test: 640×480 8bpp QueryAttr → Open → Close. 310KB work memory, all CELL_OK.
