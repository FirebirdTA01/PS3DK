# libcelpenc (cellCelpEnc) — CELP voice encoder

| Property | Value |
|---|---|
| Header | `<cell/codec/celpenc.h>` |
| Stub archive | `libcelpenc_stub.a` (nidgen) |
| SPRX module | `cellCelpEnc` |
| Entry points | 9 |
| Required sysmodules | `CELL_SYSMODULE_CELPENC` (0xf00a) |
| Decoder companion | `CELL_SYSMODULE_ADEC_CELP` (0xf019) for libadec decode |

## Surface

cellCelpEnc encodes 16 kHz PCM audio to CELP (RPE excitation) compressed voice streams. The 9-entry surface follows the standard codec QueryAttr→Open→Start→Encode→GetAu→Close lifecycle.

### Function table (9)

| Function | Signature |
|---|---|
| `cellCelpEncQueryAttr` | `int32_t(CellCelpEncAttr*)` |
| `cellCelpEncOpen` | `int32_t(CellCelpEncResource*, CellCelpEncHandle*)` |
| `cellCelpEncOpenEx` | `int32_t(CellCelpEncResourceEx*, CellCelpEncHandle*)` |
| `cellCelpEncClose` | `int32_t(CellCelpEncHandle)` |
| `cellCelpEncStart` | `int32_t(CellCelpEncHandle, CellCelpEncParam*)` |
| `cellCelpEncEnd` | `int32_t(CellCelpEncHandle)` |
| `cellCelpEncEncodeFrame` | `int32_t(CellCelpEncHandle, CellCelpEncPcmInfo*)` |
| `cellCelpEncGetAu` | `int32_t(CellCelpEncHandle, void*, CellCelpEncAuInfo*)` |
| `cellCelpEncWaitForOutput` | `int32_t(CellCelpEncHandle)` |

## Error codes

All encode in `0x806140xx`:

| Error | Value |
|---|---|
| `CELL_CELPENC_ERROR_FAILED` | 0x80614001 |
| `CELL_CELPENC_ERROR_SEQ` | 0x80614002 |
| `CELL_CELPENC_ERROR_ARG` | 0x80614003 |
| `CELL_CELPENC_ERROR_CORE_FAILED` | 0x80614081 |

## Notable ABI quirks

### 1. Encoder ↔ Decoder pairing

libcelpenc is the ENCODE side only. To DECODE the encoder's output, load `CELL_SYSMODULE_ADEC_CELP` (0xf019) and call `cellAdecOpen` with `CELL_ADEC_TYPE_CELP`. The encode and decode libraries are separate SPRX modules with independent sysmodule loads — both must be loaded for encode-decode round-trip applications.

### 2. PRXPTR audit

| Struct | PRXPTR fields |
|---|---|
| `CellCelpEncParam` | `outBuff` |
| `CellCelpEncResource` | `startAddr` |
| `CellCelpEncResourceEx` | `startAddr`, `spurs` |
| `CellCelpEncAuInfo` / `CellCelpEncPcmInfo` | `startAddr` |

### 3. Width-sensitive integers

`CellCelpEncResource.ppuThreadStackSize` is `uint32_t` (SPRX ABI). `CellCelpEncHandle` is `uint32_t` (SPRX writes 4-byte EA). No `size_t` fields remain in caller-allocated structs.

### 4. CELP framing

Input: 16 kHz mono 16-bit signed PCM. Typical frame size is 320 bytes (160 samples). The SPRX accepts a zero buffer for silent input — suitable for smoke testing.

## Sample

`samples/codec/hello-ppu-celpenc/` — PPU smoke test validated in RPCS3: loads CELL_SYSMODULE_CELPENC, queries memory (~42KB), opens an encoder instance, closes cleanly.
