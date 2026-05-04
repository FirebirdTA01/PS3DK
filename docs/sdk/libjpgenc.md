# libjpgenc (cellJpgEnc) — JPEG encoder

| Property | Value |
|---|---|
| Header | `<cell/codec/jpgenc.h>` |
| Stub archive | `libjpgenc_stub.a` (nidgen) |
| SPRX module | `cellJpgEnc` |
| Entry points | 10 |
| Required sysmodules | `CELL_SYSMODULE_JPGENC` (0x003d) |

## Surface

cellJpgEnc encodes YCbCr/RGB/Grayscale pixel data to JPEG bitstreams with configurable DCT method, compression mode (constant quality / stream size limit), sampling format, restart intervals, and marker segments.

### Function table (10)

| Function | Signature |
|---|---|
| `cellJpgEncQueryAttr` | `int32_t(const CellJpgEncConfig*, CellJpgEncAttr*)` |
| `cellJpgEncOpen` | `int32_t(const CellJpgEncConfig*, const CellJpgEncResource*, CellJpgEncHandle*)` |
| `cellJpgEncOpenEx` | `int32_t(const CellJpgEncConfig*, const CellJpgEncResourceEx*, CellJpgEncHandle*)` |
| `cellJpgEncWaitForInput` | `int32_t(CellJpgEncHandle, bool)` |
| `cellJpgEncEncodePicture` | `int32_t(CellJpgEncHandle, const CellJpgEncPicture*, const CellJpgEncEncodeParam*, const CellJpgEncOutputParam*)` |
| `cellJpgEncEncodePicture2` | (alternate with pitchWidth) |
| `cellJpgEncWaitForOutput` | `int32_t(CellJpgEncHandle, uint32_t*, bool)` |
| `cellJpgEncGetStreamInfo` | `int32_t(CellJpgEncHandle, CellJpgEncStreamInfo*)` |
| `cellJpgEncReset` | `int32_t(CellJpgEncHandle)` |
| `cellJpgEncClose` | `int32_t(CellJpgEncHandle)` |

## Notable ABI quirks

### PRXPTR audit

Same shape as libpngenc: `exParamList`, `memAddr`, `spurs`, `pictureAddr`, `markerSegmentData`, `streamAddr` all tagged. `CellJpgEncHandle` = `uint32_t`. Width-sensitive `size_t` → `uint32_t` on all struct fields.

## Sample

`samples/codec/hello-ppu-jpgenc/` — PPU smoke test: 640×480 YCbCr 4:2:0 QueryAttr → Open → Close. 136KB work memory, all CELL_OK.
