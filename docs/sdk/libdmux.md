# libdmux (cellDmux) — PAMF stream demultiplexer

| Property | Value |
|---|---|
| Header | `<cell/codec/dmux.h>`, `<cell/codec/dmux_pamf.h>` |
| Stub archive | `libdmux_stub.a` (nidgen) |
| Entry points | 20 |
| Required sysmodules | `CELL_SYSMODULE_DMUX` (0x0007) + `CELL_SYSMODULE_DMUX_PAMF` (0x002a) for PAMF streams |

## Surface

cellDmux demuxes PAMF (and other container) streams into elementary streams (ES), delivering access units via callback. The surface decomposes into:

| Area | Functions |
|---|---|
| **Query** | `cellDmuxQueryAttr`, `cellDmuxQueryAttr2`, `cellDmuxQueryEsAttr`, `cellDmuxQueryEsAttr2` |
| **Lifecycle** | `cellDmuxOpen`, `cellDmuxOpenEx`, `cellDmuxOpen2`, `cellDmuxClose` |
| **Stream feed** | `cellDmuxSetStream`, `cellDmuxResetStream`, `cellDmuxResetStreamAndWaitDone` |
| **ES management** | `cellDmuxEnableEs`, `cellDmuxDisableEs`, `cellDmuxResetEs` |
| **Access unit** | `cellDmuxGetAu`, `cellDmuxGetAuEx`, `cellDmuxPeekAu`, `cellDmuxPeekAuEx`, `cellDmuxReleaseAu` |
| **Flush** | `cellDmuxFlushEs` |

### Function table

| Function | Signature |
|---|---|
| `cellDmuxQueryAttr` | `int32_t(const CellDmuxType*, CellDmuxAttr*)` |
| `cellDmuxQueryAttr2` | `int32_t(const CellDmuxType2*, CellDmuxAttr*)` |
| `cellDmuxOpen` | `int32_t(const CellDmuxType*, const CellDmuxResource*, const CellDmuxCb*, CellDmuxHandle*)` |
| `cellDmuxOpenEx` | `int32_t(const CellDmuxType*, const CellDmuxResourceEx*, const CellDmuxCb*, CellDmuxHandle*)` |
| `cellDmuxOpen2` | `int32_t(const CellDmuxType2*, const CellDmuxResource2*, const CellDmuxCb*, CellDmuxHandle*)` |
| `cellDmuxClose` | `int32_t(CellDmuxHandle)` |
| `cellDmuxSetStream` | `int32_t(CellDmuxHandle, const void*, size_t, bool, uint64_t)` |
| `cellDmuxResetStream` | `int32_t(CellDmuxHandle)` |
| `cellDmuxResetStreamAndWaitDone` | `int32_t(CellDmuxHandle)` |
| `cellDmuxQueryEsAttr` | `int32_t(const CellDmuxType*, const CellCodecEsFilterId*, const void*, CellDmuxEsAttr*)` |
| `cellDmuxQueryEsAttr2` | `int32_t(const CellDmuxType2*, const CellCodecEsFilterId*, const void*, CellDmuxEsAttr*)` |
| `cellDmuxEnableEs` | `int32_t(CellDmuxHandle, const CellCodecEsFilterId*, const CellDmuxEsResource*, const CellDmuxEsCb*, const void*, CellDmuxEsHandle*)` |
| `cellDmuxDisableEs` | `int32_t(CellDmuxEsHandle)` |
| `cellDmuxGetAu` | `int32_t(CellDmuxEsHandle, const CellDmuxAuInfo**, void**)` |
| `cellDmuxPeekAu` | `int32_t(CellDmuxEsHandle, const CellDmuxAuInfo**, void**)` |
| `cellDmuxGetAuEx` | `int32_t(CellDmuxEsHandle, const CellDmuxAuInfoEx**, void**)` |
| `cellDmuxPeekAuEx` | `int32_t(CellDmuxEsHandle, const CellDmuxAuInfoEx**, void**)` |
| `cellDmuxReleaseAu` | `int32_t(CellDmuxEsHandle)` |
| `cellDmuxFlushEs` | `int32_t(CellDmuxEsHandle)` |
| `cellDmuxResetEs` | `int32_t(CellDmuxEsHandle)` |

## Error codes

All errors encode in `0x806102xx` (facility `CELL_ERROR_FACILITY_CODEC` = 0x061, libdmux range 0x0201–0x02ff):

| Error | Value |
|---|---|
| `CELL_DMUX_ERROR_ARG` | 0x80610201 |
| `CELL_DMUX_ERROR_SEQ` | 0x80610202 |
| `CELL_DMUX_ERROR_BUSY` | 0x80610203 |
| `CELL_DMUX_ERROR_EMPTY` | 0x80610204 |
| `CELL_DMUX_ERROR_FATAL` | 0x80610205 |

## Initialisation lifecycle

1. Load sysmodules: `cellSysmoduleLoadModule(CELL_SYSMODULE_DMUX)` then `cellSysmoduleLoadModule(CELL_SYSMODULE_DMUX_PAMF)` (for PAMF streams).
2. Query memory requirements: `cellDmuxQueryAttr(&type, &attr)` — returns `attr.memSize`.
3. Allocate memory of `attr.memSize` bytes (16-byte aligned).
4. Instantiate demuxer: `cellDmuxOpen(&type, &resource, &cb, &handle)`.
5. Feed stream data: `cellDmuxSetStream` or enable an ES with `cellDmuxEnableEs`.
6. Pull access units: `cellDmuxGetAu` / `cellDmuxGetAuEx` → `cellDmuxReleaseAu`.
7. Tear down: `cellDmuxClose(handle)`.

## Notable ABI quirks

### 1. Width-sensitive integers in caller-allocated structs

The SPRX is built with a 32-bit-pointer ABI where `size_t` is 4 bytes. Our LP64 PPU headers must use `uint32_t` for every integer-width field the SPRX reads or writes in caller-allocated structs. Affected fields:

| Struct | Fields |
|---|---|
| `CellDmuxAttr` | `memSize` |
| `CellDmuxEsAttr` | `memSize` |
| `CellDmuxResource` | `memSize`, `ppuThreadStackSize` |
| `CellDmuxResourceEx` | `memSize`, `ppuThreadStackSize` |
| `CellDmuxEsResource` | `memSize` |
| `CellDmuxAuInfo` | `auSize`, `auMaxSize` |
| `CellDmuxAuInfoEx` | `auSize` |
| `CellDmuxPamfSpecificInfo` | `thisSize` |

Using LP64 `size_t` (8 bytes) would misalign all subsequent fields — for example, `CellDmuxAttr.memSize` at offset 0 would swallow the SPRX's 32-bit `demuxerVerUpper` value into its high 32 bits, producing garbage sizes like `0x3E80_0008_0000_0000`.

### 2. Required sysmodules

| Sysmodule | ID | Purpose |
|---|---|---|
| `CELL_SYSMODULE_DMUX` | 0x0007 | libdmux base SPRX |
| `CELL_SYSMODULE_DMUX_PAMF` | 0x002a | PAMF backend core-ops table |
| `CELL_SYSMODULE_DMUX_AL` | 0x002d | AL-PAMF backend (sister) |

Load BOTH DMUX + DMUX_PAMF before any `cellDmux*` call. Missing the PAMF module causes `cellDmuxOpen` to return `CELL_DMUX_ERROR_ARG` (0x80610201) — a misleading error code for a missing-dependency condition, because the internal `get_core_ops()` lookup fails and the error gets folded through `get_error()`.

### 3. Required CellDmuxResource field ranges

The PAMF backend's `open` path (hidden two layers below `cellDmuxOpen`) enforces:

| Field | Constraint |
|---|---|
| `numOfSpus` | **== 1** (exactly one SPU) |
| `ppuThreadStackSize` | **>= 0x1000** (4 KB minimum; 16 KB recommended) |
| `ppuThreadPriority` | **< 0xc00** |
| `spuThreadPriority` | **< 0x100** |
| `memSize` | **>= sizeof(DmuxPamfContext) + 0xe7b** (the `QueryAttr`-returned value covers this) |
| `memAddr` | **non-NULL**, 16-byte aligned (hardware enforces; some HLE does not) |

Violations at this layer collapse to `CELL_DMUX_ERROR_ARG`.

### 4. Callback non-NULL requirement

`cellDmuxOpen` rejects `cb.cbFunc == NULL` with `CELL_DMUX_ERROR_ARG`. A no-op `CellDmuxCbMsg` callback is sufficient:

```c
static uint32_t dmux_msg_cb(CellDmuxHandle h, const CellDmuxMsg *msg, void *arg)
{
    (void)h; (void)msg; (void)arg;
    return CELL_OK;
}
```

### 5. Width-sensitive output parameters

Functions like `cellDmuxGetAu` take `void **auSpecificInfo`-style output-pointer parameters. The SPRX writes 32 bits into a slot the LP64 caller declares 64 bits wide — the high 32 bits of the caller's slot remain uninitialised. Callers that cast or use the value as a generic pointer may read junk in the high half.

**Workaround:** declare the receiving variable as `uintptr_t` (or pre-zero it) before passing its address.

### Opaque handles

`CellDmuxHandle` and `CellDmuxEsHandle` are typed as `uint32_t`. The SPRX writes a 4-byte effective address into `*handle` at create time. Callers must treat these as opaque tokens (never dereference).

## Sample

`samples/codec/hello-ppu-dmux/` — PPU smoke test validated in RPCS3: loads DMUX + DMUX_PAMF sysmodules, queries PAMF demuxer memory requirements, opens a demuxer instance, closes it, and exits cleanly.
