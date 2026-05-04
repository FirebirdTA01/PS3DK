# libatrac3plus (cellAtrac) — ATRAC3+ audio codec

| Property | Value |
|---|---|
| Header | `<cell/codec/libatrac3plus.h>` |
| Stub archive | `libatrac3plus_stub.a` (nidgen) |
| SPRX module | `cellAtrac` |
| Entry points | 23 |
| Required sysmodules | `CELL_SYSMODULE_ATRAC3PLUS` (0x0013) |

## Surface

cellAtrac is the standalone ATRAC3+ audio decoder. It operates independently of libadec's framework — it has its own CreateDecoder/DeleteDecoder/Decode lifecycle with stream-data management and loop/sample queries. It also integrates into libadec as a sub-decoder via `CELL_ADEC_TYPE_ATRACX` / `CELL_ADEC_TYPE_ATRACX_2CH` etc.

### Function table (23)

| Area | Functions |
|---|---|
| Setup | `cellAtracSetDataAndGetMemSize`, `cellAtracCreateDecoder`, `cellAtracCreateDecoderExt`, `cellAtracDeleteDecoder` |
| Decode | `cellAtracDecode`, `cellAtracAddStreamData` |
| Buffer | `cellAtracGetSecondBufferInfo`, `cellAtracSetSecondBuffer`, `cellAtracGetStreamDataInfo`, `cellAtracGetVacantSize`, `cellAtracIsSecondBufferNeeded` |
| Info | `cellAtracGetRemainFrame`, `cellAtracGetChannel`, `cellAtracGetMaxSample`, `cellAtracGetNextSample`, `cellAtracGetSoundInfo`, `cellAtracGetNextDecodePosition`, `cellAtracGetBitrate`, `cellAtracGetLoopInfo` |
| Seek/Reset | `cellAtracSetLoopNum`, `cellAtracGetBufferInfoForResetting`, `cellAtracResetPlayPosition` |
| Debug | `cellAtracGetInternalErrorInfo` |

## Error codes

All encode in `0x806103xx` (facility `CELL_ERROR_FACILITY_CODEC` = 0x061):

| Error | Value |
|---|---|
| `CELL_ATRAC_ERROR_API_FAIL` | 0x80610301 |
| `CELL_ATRAC_ERROR_UNKNOWN_FORMAT` | 0x80610312 |
| `CELL_ATRAC_ERROR_NO_DECODER` | 0x80610321 |
| `CELL_ATRAC_ERROR_NODATA_IN_BUFFER` | 0x80610332 |
| `CELL_ATRAC_ERROR_NEED_SECOND_BUFFER` | 0x80610334 |

(Full set of 20+ codes documented in the header.)

## Initialisation lifecycle

1. Load sysmodule: `cellSysmoduleLoadModule(CELL_SYSMODULE_ATRAC3PLUS)`.
2. Query memory: `cellAtracSetDataAndGetMemSize(&handle, buf, readByte, bufByte, &workSize)`.
3. Allocate work memory, create decoder: `cellAtracCreateDecoder(&handle, workMem, ppuPrio, spuPrio)`.
4. Decode loop: `cellAtracDecode(&handle, outBuf, &samples, &finish, &remain)`.
5. Tear down: `cellAtracDeleteDecoder(&handle)`.

## Notable ABI quirks

### 1. PRXPTR audit

| Struct | PRXPTR fields |
|---|---|
| `CellAtracBufferInfo` | `pucWriteAddr` |
| `CellAtracExtRes` | `pSpurs` |

`CellAtracHandle` is a fixed 512-byte opaque struct — no pointer fields.

### 2. Codec-plugin duality

cellAtrac serves double duty: it can be called standalone (direct `cellAtracDecode` etc.) OR plugged into libadec as a sub-decoder via `cellAdecOpen` with `CELL_ADEC_TYPE_ATRACX` (and the per-channel variants `_ATRACX_2CH`, `_6CH`, `_8CH`). The standalone API surface is richer (23 entry points vs. libadec's framework of 9).

### 3. Handle is a struct, not a pointer

`CellAtracHandle` is a 512-byte caller-allocated struct (`uint8_t uiWorkMem[512]` aligned to 8 bytes). The SPRX writes internal decoder state into this block. Callers must zero-initialize before first use and preserve the contents across calls.

## Sample

`samples/codec/hello-ppu-atrac3plus/` — PPU smoke test: loads CELL_SYSMODULE_ATRAC3PLUS, queries memory size, creates a decoder instance, deletes it.
