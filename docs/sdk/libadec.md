# libadec (cellAdec) — audio decoder framework

| Property | Value |
|---|---|
| Headers | `<cell/codec/adec.h>`, `<cell/codec/adec_base.h>` |
| Stub archive | `libadec_stub.a` (nidgen) |
| SPRX module | `cellAdec` |
| Entry points | 9 |
| Required sysmodules | `CELL_SYSMODULE_ADEC` (0x0006) |

## Surface

cellAdec is the audio decoder framework. Concrete codecs (Atrac3+, AAC, MP3, CELP, M4AAC, LPCM) plug in as sub-decoders via `CellAdecType.audioCodecType` at `cellAdecOpen` time.

### Function table (9)

| Function | Signature |
|---|---|
| `cellAdecQueryAttr` | `int32_t(CellAdecType*, CellAdecAttr*)` |
| `cellAdecOpen` | `int32_t(CellAdecType*, CellAdecResource*, CellAdecCb*, CellAdecHandle*)` |
| `cellAdecOpenEx` | `int32_t(CellAdecType*, CellAdecResourceEx*, CellAdecCb*, CellAdecHandle*)` |
| `cellAdecClose` | `int32_t(CellAdecHandle)` |
| `cellAdecStartSeq` | `int32_t(CellAdecHandle, void *param)` |
| `cellAdecEndSeq` | `int32_t(CellAdecHandle)` |
| `cellAdecDecodeAu` | `int32_t(CellAdecHandle, CellAdecAuInfo*)` |
| `cellAdecGetPcm` | `int32_t(CellAdecHandle, void *outBuff)` |
| `cellAdecGetPcmItem` | `int32_t(CellAdecHandle, const CellAdecPcmItem**)` |

### Codec type constants

| Constant | Value | Description |
|---|---|---|
| `CELL_ADEC_TYPE_LPCM_PAMF` | 1 | LPCM (PAMF container) |
| `CELL_ADEC_TYPE_AC3` | 2 | Dolby AC-3 |
| `CELL_ADEC_TYPE_ATRACX` | 3 | ATRAC3+ (libatrac3plus) |
| `CELL_ADEC_TYPE_MP3` | 4 | MPEG-1 Layer 3 |
| `CELL_ADEC_TYPE_ATRAC3` | 5 | ATRAC3 |
| `CELL_ADEC_TYPE_MPEG_L2` | 6 | MPEG-1/2 Layer 2 |
| `CELL_ADEC_TYPE_CELP` | 11 | CELP (libcelpenc) |
| `CELL_ADEC_TYPE_ATRACX_2CH` | 13 | ATRAC3+ 2-channel |
| `CELL_ADEC_TYPE_ATRACX_6CH` | 14 | ATRAC3+ 6-channel |
| `CELL_ADEC_TYPE_ATRACX_8CH` | 15 | ATRAC3+ 8-channel |
| `CELL_ADEC_TYPE_M4AAC` | 16 | MPEG-4 AAC |
| `CELL_ADEC_TYPE_CELP8` | 24 | CELP8 (libcelp8enc) |

## Error codes

All encode in `0x806100xx` (facility `CELL_ERROR_FACILITY_CODEC` = 0x061):

| Error | Value |
|---|---|
| `CELL_ADEC_ERROR_FATAL` | 0x80610001 |
| `CELL_ADEC_ERROR_SEQ` | 0x80610002 |
| `CELL_ADEC_ERROR_ARG` | 0x80610003 |
| `CELL_ADEC_ERROR_BUSY` | 0x80610004 |
| `CELL_ADEC_ERROR_EMPTY` | 0x80610005 |

## Initialisation lifecycle

1. Load sysmodule: `cellSysmoduleLoadModule(CELL_SYSMODULE_ADEC)`.
2. Query memory: `cellAdecQueryAttr(&type, &attr)` → returns `workMemSize`.
3. Allocate work memory, open decoder: `cellAdecOpen(&type, &res, &cb, &handle)`.
4. Start a sequence (optional codec-specific params): `cellAdecStartSeq(handle, param)`.
5. Decode loop: `cellAdecDecodeAu(handle, &auInfo)` → callback fires `CELL_ADEC_MSG_TYPE_PCMOUT` → `cellAdecGetPcm(handle, outBuff)`.
6. End sequence: `cellAdecEndSeq(handle)`, Close: `cellAdecClose(handle)`.

## Notable ABI quirks

### 1. Codec-plugin architecture

libadec is a framework host. Individual codecs (Atrac3+, AAC, MP3, CELP, M4AAC) live in separate SPRX modules and plug in via `cellAdecOpen`'s `CellAdecType.audioCodecType`. Opening a codec type not yet loaded returns `CELL_ADEC_ERROR_ARG`. The full decode stack typically involves: PAMF demux → `cellPamfStreamTypeToAdecCodecType()` → `cellAdecOpen`.

### 2. PRXPTR audit

| Struct | PRXPTR fields |
|---|---|
| `CellAdecResource` | `startAddr` |
| `CellAdecResourceEx` | `startAddr`, `spurs` |
| `CellAdecCb` | `callbackFunc`, `callbackArg` |
| `CellAdecAuInfo` | `startAddr` |
| `CellAdecPcmAttr` | `bsiInfo` |
| `CellAdecPcmItem` | `startAddr` |

### 3. Width-sensitive integers

`CellAdecResource.ppuThreadStackSize` and `CellAdecResourceEx.ppuThreadStackSize` are `uint32_t` (SPRX ABI width). No other width-sensitive fields in caller-allocated structs.

### 4. Opaque handle

`CellAdecHandle` is `uint32_t` — the SPRX writes a 4-byte EA into `*handle` at `cellAdecOpen` time.

### 5. Callback non-NULL requirement

`cellAdecOpen` rejects `cb.callbackFunc == NULL` with `CELL_ADEC_ERROR_ARG`. A no-op `CellAdecCbMsg` is sufficient for smoke tests.

### 6. Shared types

`CellAdecTimeStamp` = `CellCodecTimeStamp` (from `<cell/codec/types.h>`). `CELL_ADEC_PTS_INVALID` = `CELL_CODEC_PTS_INVALID`. The `cellPamfStreamTypeToAdecCodecType` macro in `<cell/codec/pamf.h>` references these types and activates when `<cell/codec/adec.h>` is in the include tree.

## Sample

`samples/codec/hello-ppu-adec/` — PPU smoke test validated in RPCS3: loads CELL_SYSMODULE_ADEC, queries LPCM decoder memory requirements, opens a decoder instance, closes it cleanly.
