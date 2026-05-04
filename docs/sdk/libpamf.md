# libpamf (cellPamf) — PAMF container parser

| Property | Value |
|---|---|
| Header | `<cell/codec/pamf.h>` |
| Stub archive | `libpamf_stub.a` (nidgen) |
| SPRX module | `cellPamf` |
| Entry points | 23 |
| Required sysmodules | `CELL_SYSMODULE_PAMF` (0x0012) |

## Surface

cellPamf parses PAMF (PlayStation Advanced Media Format) container headers, exposes EP (entry-point) tables and per-stream metadata, and maps PAMF stream types to audio/video decoder codec types.

### Function table (23)

| Function | Signature |
|---|---|
| `cellPamfGetHeaderSize` | `int(const uint8_t*, uint64_t, uint64_t*)` |
| `cellPamfGetHeaderSize2` | `int(const uint8_t*, uint64_t, uint32_t, uint64_t*)` |
| `cellPamfGetStreamOffsetAndSize` | `int(const uint8_t*, uint64_t, uint64_t*, uint64_t*)` |
| `cellPamfVerify` | `int(const uint8_t*, uint64_t)` |
| `cellPamfReaderInitialize` | `int(CellPamfReader*, const uint8_t*, uint64_t, uint32_t)` |
| `cellPamfReaderGetPresentationStartTime` | `int(CellPamfReader*, CellCodecTimeStamp*)` |
| `cellPamfReaderGetPresentationEndTime` | `int(CellPamfReader*, CellCodecTimeStamp*)` |
| `cellPamfReaderGetMuxRateBound` | `uint32_t(CellPamfReader*)` |
| `cellPamfReaderGetNumberOfStreams` | `uint8_t(CellPamfReader*)` |
| `cellPamfReaderGetNumberOfSpecificStreams` | `uint8_t(CellPamfReader*, uint8_t)` |
| `cellPamfReaderSetStreamWithIndex` | `int(CellPamfReader*, uint8_t)` |
| `cellPamfReaderSetStreamWithTypeAndChannel` | `int(CellPamfReader*, uint8_t, uint8_t)` |
| `cellPamfStreamTypeToEsFilterId` | `int(uint8_t, uint8_t, CellCodecEsFilterId*)` |
| `cellPamfReaderSetStreamWithTypeAndIndex` | `int(CellPamfReader*, uint8_t, uint8_t)` |
| `cellPamfReaderGetStreamIndex` | `int(CellPamfReader*)` |
| `cellPamfReaderGetStreamTypeAndChannel` | `int(CellPamfReader*, uint8_t*, uint8_t*)` |
| `cellPamfReaderGetEsFilterId` | `int(CellPamfReader*, CellCodecEsFilterId*)` |
| `cellPamfReaderGetStreamInfo` | `int(CellPamfReader*, void*, uint32_t)` |
| `cellPamfReaderGetNumberOfEp` | `uint32_t(CellPamfReader*)` |
| `cellPamfReaderGetEpIteratorWithIndex` | `int(CellPamfReader*, uint32_t, CellPamfEpIterator*)` |
| `cellPamfReaderGetEpIteratorWithTimeStamp` | `int(CellPamfReader*, CellCodecTimeStamp*, CellPamfEpIterator*)` |
| `cellPamfEpIteratorGetEp` | `int(CellPamfEpIterator*, CellPamfEp*)` |
| `cellPamfEpIteratorMove` | `int(CellPamfEpIterator*, int, CellPamfEp*)` |

### Stream type constants

| Constant | Value | Description |
|---|---|---|
| `CELL_PAMF_STREAM_TYPE_AVC` | 0 | H.264 video |
| `CELL_PAMF_STREAM_TYPE_M2V` | 1 | MPEG-2 video |
| `CELL_PAMF_STREAM_TYPE_ATRAC3PLUS` | 2 | ATRAC3+ audio |
| `CELL_PAMF_STREAM_TYPE_PAMF_LPCM` | 3 | LPCM audio |
| `CELL_PAMF_STREAM_TYPE_AC3` | 4 | Dolby AC-3 audio |
| `CELL_PAMF_STREAM_TYPE_USER_DATA` | 5 | Opaque user data |

## Error codes

All encode in `0x806105xx` (facility `CELL_ERROR_FACILITY_CODEC` = 0x061):

| Error | Value |
|---|---|
| `CELL_PAMF_ERROR_STREAM_NOT_FOUND` | 0x80610501 |
| `CELL_PAMF_ERROR_INVALID_PAMF` | 0x80610502 |
| `CELL_PAMF_ERROR_INVALID_ARG` | 0x80610503 |
| `CELL_PAMF_ERROR_UNKNOWN_TYPE` | 0x80610504 |
| `CELL_PAMF_ERROR_UNSUPPORTED_VERSION` | 0x80610505 |
| `CELL_PAMF_ERROR_UNKNOWN_STREAM` | 0x80610506 |
| `CELL_PAMF_ERROR_EP_NOT_FOUND` | 0x80610507 |
| `CELL_PAMF_ERROR_NOT_AVAILABLE` | 0x80610508 |

## Initialisation lifecycle

1. Load sysmodule: `cellSysmoduleLoadModule(CELL_SYSMODULE_PAMF)`.
2. Parse raw PAMF header: `cellPamfVerify` / `cellPamfGetHeaderSize` / `cellPamfGetStreamOffsetAndSize`.
3. Initialize reader: `cellPamfReaderInitialize`.
4. Query streams: `cellPamfReaderGetNumberOfStreams`, `cellPamfReaderSetStreamWithIndex`, `cellPamfReaderGetStreamInfo`.
5. Iterate EP table: `cellPamfReaderGetNumberOfEp` → `cellPamfReaderGetEpIteratorWithIndex` → `cellPamfEpIteratorMove` / `cellPamfEpIteratorGetEp`.

## Notable ABI quirks

### 1. PRXPTR audit

`CellPamfEpIterator.pCur` (const void*) carries `ATTRIBUTE_PRXPTR` — the SPRX writes a 4-byte EA into this field during EP-table iteration. No other struct fields require PRXPTR (all scalars or fixed-width integers).

### 2. PAMF magic

The PAMF magic is the ASCII string `"PAMF"` (`0x50414D46` big-endian) at offset 0. The SPRX validates this before attempting any parse; a buffer without the magic returns `CELL_PAMF_ERROR_INVALID_PAMF`.

### 3. Shared codec types

`CellPamfTimeStamp` is a typedef for `CellCodecTimeStamp` (from `<cell/codec/types.h>`, shipped with libdmux). `CellPamfEsFilterId` is a typedef for `CellCodecEsFilterId`. Both are in-tree.

### 4. Codec-type converter macros

Two convenience macros (`cellPamfStreamTypeToVdecCodecType`, `cellPamfStreamTypeToAdecCodecType`) are `#ifdef`-guarded pending `<cell/codec/vdec.h>` and `<cell/codec/adec.h>`. They become active automatically when those headers are installed in later cycles.

### 5. Width-sensitive integers

No `size_t` / `time_t` / `off_t` fields in caller-allocated structs (all fields are `uint32_t`, `uint64_t`, `uint8_t`, or `bool`). `cellPamfReaderGetStreamInfo`'s `size` parameter is declared `uint32_t` for conservative correctness; function-argument `size_t` is actually ABI-safe (PowerPC register passing), but the explicit `uint32_t` avoids any ambiguity.

## Sample

`samples/codec/hello-ppu-pamf/` — PPU smoke test validated in RPCS3: loads CELL_SYSMODULE_PAMF, feeds a minimal PAMF-magic buffer, exercises Verify / GetHeaderSize / GetStreamOffsetAndSize / ReaderInitialize / GetNumberOfStreams. The SPRX returns `CELL_PAMF_ERROR_UNSUPPORTED_VERSION` (0x80610505) for the malformed buffer — expected, proves link surface works.
