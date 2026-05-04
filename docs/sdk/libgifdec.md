# libgifdec (cellGifDec) — GIF decoder

| Property | Value |
|---|---|
| Header | `<cell/codec/gifdec.h>` |
| Stub archive | `libgifdec_stub.a` (nidgen) |
| SPRX module | `cellGifDec` |
| Entry points | 12 |
| Required sysmodules | `CELL_SYSMODULE_GIFDEC` (0xf010) |

## Surface

cellGifDec decodes GIF87a/89a bitstreams with optional SPU acceleration. 12 entry points across base + Ext variants.

### Function table (12)

| Function | Signature |
|---|---|
| `cellGifDecCreate` | `int32_t(CellGifDecMainHandle*, const CellGifDecThreadInParam*, CellGifDecThreadOutParam*)` |
| `cellGifDecExtCreate` | (Create with SPURS Ext support) |
| `cellGifDecOpen` | `int32_t(CellGifDecMainHandle, CellGifDecSubHandle*, const CellGifDecSrc*, CellGifDecOpnInfo*)` |
| `cellGifDecExtOpen` | (Open with stream callback) |
| `cellGifDecReadHeader` | `int32_t(CellGifDecMainHandle, CellGifDecSubHandle, CellGifDecInfo*)` |
| `cellGifDecExtReadHeader` | (ReadHeader with ext info) |
| `cellGifDecSetParameter` | `int32_t(CellGifDecMainHandle, CellGifDecSubHandle, const CellGifDecInParam*, CellGifDecOutParam*)` |
| `cellGifDecExtSetParameter` | (SetParameter with ext in/out) |
| `cellGifDecDecodeData` | `int32_t(CellGifDecMainHandle, CellGifDecSubHandle, uint8_t*, const CellGifDecDataCtrlParam*, CellGifDecDataOutInfo*)` |
| `cellGifDecExtDecodeData` | (DecodeData with display callback) |
| `cellGifDecClose` | `int32_t(CellGifDecMainHandle, CellGifDecSubHandle)` |
| `cellGifDecDestroy` | `int32_t(CellGifDecMainHandle)` |

## Notable ABI quirks

### PRXPTR audit

Callback function pointers (cbCtrlMallocFunc, cbCtrlFreeFunc, cbCtrlStrmFunc, cbCtrlDispFunc) and data pointers (streamPtr, data, outputImage, spurs) all tagged. `CellGifDecMainHandle`/`SubHandle` = `uint32_t`.

### Two-level handle

cellGifDec uses a MainHandle (Create/Destroy) + SubHandle (Open/Close) model — different from the single-handle patterns in pngenc/jpgenc.

## Sample

`samples/codec/hello-ppu-gifdec/` — PPU smoke test: Create → Open with valid GIF89a buffer → Close → Destroy. All calls return defined facility errors, SPRX processes the buffer successfully.
