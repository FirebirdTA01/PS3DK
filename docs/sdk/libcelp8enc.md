# libcelp8enc (cellCelp8Enc) — CELP-8 voice encoder

| Property | Value |
|---|---|
| Header | `<cell/codec/celp8enc.h>` |
| Stub archive | `libcelp8enc_stub.a` (nidgen) |
| SPRX module | `cellCelp8Enc` |
| Entry points | 9 |
| Required sysmodules | `CELL_SYSMODULE_CELP8ENC` (0x0048) |
| Decoder companion | `CELL_SYSMODULE_ADEC_CELP8` (0x0047) for libadec decode |

## Surface

cellCelp8Enc encodes 8 kHz PCM audio to CELP-8 (MPE excitation) compressed voice streams. The API surface is structurally identical to cellCelpEnc (celpenc) with its own type namespace and error-code range (0x806140a1 vs. celpenc's 0x80614001).

### Function table (9)

| Function | Signature |
|---|---|
| `cellCelp8EncQueryAttr` | `int32_t(CellCelp8EncAttr*)` |
| `cellCelp8EncOpen` | `int32_t(CellCelp8EncResource*, CellCelp8EncHandle*)` |
| `cellCelp8EncOpenEx` | `int32_t(CellCelp8EncResourceEx*, CellCelp8EncHandle*)` |
| `cellCelp8EncClose` | `int32_t(CellCelp8EncHandle)` |
| `cellCelp8EncStart` | `int32_t(CellCelp8EncHandle, CellCelp8EncParam*)` |
| `cellCelp8EncEnd` | `int32_t(CellCelp8EncHandle)` |
| `cellCelp8EncEncodeFrame` | `int32_t(CellCelp8EncHandle, CellCelp8EncPcmInfo*)` |
| `cellCelp8EncGetAu` | `int32_t(CellCelp8EncHandle, void*, CellCelp8EncAuInfo*)` |
| `cellCelp8EncWaitForOutput` | `int32_t(CellCelp8EncHandle)` |

## Error codes

All encode in `0x806140ax/0x806140bx`:

| Error | Value |
|---|---|
| `CELL_CELP8ENC_ERROR_FAILED` | 0x806140a1 |
| `CELL_CELP8ENC_ERROR_SEQ` | 0x806140a2 |
| `CELL_CELP8ENC_ERROR_ARG` | 0x806140a3 |
| `CELL_CELP8ENC_ERROR_CORE_FAILED` | 0x806140b1 |

## Notable ABI quirks

### 1. Distinguish from celpenc

cellCelp8Enc has its own error namespace (0x806140ax vs. celpenc's 0x8061400x), its own sysmodule (0x0048 vs. 0xf00a), its own handle type (CellCelp8EncHandle), and MPE excitation (vs. celpenc's RPE). The two are independent SPRX modules — you can't use a cellCelp8Enc handle with cellCelpEnc entry points or vice versa.

### 2. CELP8 framing

Input: 8 kHz mono float PCM. The SPRX uses MPE excitation with configurable pulse-density (MPE_CONFIG_0 through MPE_CONFIG_26). Smoke samples can use a zero buffer for silent input.

### 3. Encoder ↔ Decoder pairing

To decode CELP-8 output, load `CELL_SYSMODULE_ADEC_CELP8` (0x0047) and call `cellAdecOpen` with `CELL_ADEC_TYPE_CELP8`.

## Sample

`samples/codec/hello-ppu-celp8enc/` — PPU smoke test validated in RPCS3: loads CELL_SYSMODULE_CELP8ENC, queries memory (~47KB), opens an encoder instance, closes cleanly.
