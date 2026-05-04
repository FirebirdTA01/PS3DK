# libatrac3multi (cellAtracMulti) — multi-channel ATRAC3 decoder

| Property | Value |
|---|---|
| Header | `<cell/codec/libatrac3multi.h>` |
| Stub archive | `libatrac3multi_stub.a` (nidgen) |
| SPRX module | `cellAtracMulti` |
| Entry points | 24 |
| Required sysmodules | `CELL_SYSMODULE_LIBATRAC3MULTI` (0xf054) |

## Surface

cellAtracMulti is the multi-channel ATRAC3 decoder (5.1 / 7.1 surround). Its API surface is nearly identical to cellAtrac (atrac3plus) with one addition: `cellAtracMultiGetChannelType` returns the per-channel type enum (LR, LSRS, C, LFE, etc.) for surround channel mapping. The error-code namespace is distinct (0x80610bxx vs. atrac3plus's 0x806103xx).

### Function table (24)

24 entry points — same shape as cellAtrac (SetDataAndGetMemSize, CreateDecoder, CreateDecoderExt, DeleteDecoder, Decode, AddStreamData, Get/Buffer info, loop/sample queries, reset/seek, internal error) plus `cellAtracMultiGetChannelType`.

## Error codes

All encode in `0x80610bxx`:

| Error | Value |
|---|---|
| `CELL_ATRACMULTI_ERROR_API_FAIL` | 0x80610b01 |
| `CELL_ATRACMULTI_ERROR_UNKNOWN_FORMAT` | 0x80610b12 |
| `CELL_ATRACMULTI_ERROR_NO_DECODER` | 0x80610b21 |
| `CELL_ATRACMULTI_ERROR_API_PARAMETER` | 0x80610b91 |

(Full set of 24+ codes in the header — mirrors atrac3plus shape with its own number range.)

## Notable ABI quirks

### 1. Distinguish from atrac3plus

cellAtracMulti has its own error namespace (0x80610bxx vs. atrac3plus's 0x806103xx), its own sysmodule (0xf054 vs. 0x0013), and its own handle type (CellAtracMultiHandle, 512 bytes). The API surface is identical in shape to atrac3plus but the two are independent SPRX modules — you can't use a cellAtracMulti handle with cellAtrac entry points or vice versa.

### 2. Channel-type query

`cellAtracMultiGetChannelType` extends the base API with surround-channel mapping. Returns `CellAtracMultiChannelType` enum: LR, LSRS, C, LFE, LR2, LSRS2 — used for 5.1/7.1 speaker routing.

### 3. libadec integration

Multi-channel ATRAC3 streams plug into libadec via `CELL_ADEC_TYPE_ATRACX_2CH` / `_6CH` / `_8CH` at `cellAdecOpen`. The standalone API is used when direct decoder control is needed (loop points, bitrate queries, reset).

## Sample

`samples/codec/hello-ppu-atrac3multi/` — PPU smoke test: loads CELL_SYSMODULE_LIBATRAC3MULTI, queries memory, creates a decoder, deletes it.
