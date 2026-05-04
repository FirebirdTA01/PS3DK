# libvpost (cellVpost) — video post-processing

| Property | Value |
|---|---|
| Header | `<cell/vpost.h>` |
| Stub archive | `libvpost_stub.a` (nidgen) |
| SPRX module | `cellVpost` |
| Entry points | 5 |
| Required sysmodules | `CELL_SYSMODULE_VPOST` (0x0008) |

## Surface

cellVpost provides hardware-accelerated video post-processing: colour-space conversion (CSC), video scaling (VSC), and interlace-to-progressive conversion (IPC). The five-entry surface follows the standard codec QueryAttr→Open→Exec→Close lifecycle.

### Function table (5)

| Function | Signature |
|---|---|
| `cellVpostQueryAttr` | `int32_t(const CellVpostCfgParam*, CellVpostAttr*)` |
| `cellVpostOpen` | `int32_t(const CellVpostCfgParam*, const CellVpostResource*, CellVpostHandle*)` |
| `cellVpostOpenEx` | `int32_t(const CellVpostCfgParam*, const CellVpostResourceEx*, CellVpostHandle*)` |
| `cellVpostClose` | `int32_t(CellVpostHandle)` |
| `cellVpostExec` | `int32_t(CellVpostHandle, const CellVpostCtrlParam*, const CellVpostPictureInfo*, CellVpostPictureInfo*)` |

### Processing pipeline

1. **CSC** — colour-space conversion (YUV→RGBA, RGBA→YUV).
2. **VSC** — video scaling with selectable algorithm (bilinear, linear-sharp, 2×4-tap, 8×4-tap).
3. **IPC** — interlace-to-progressive conversion (line doubling, averaging, matched averaging).

The `CellVpostCfgParam` determines input/output picture dimensions, formats, and depths. `CellVpostCtrlParam` specifies the per-frame processing chain: execution type (progressive/interlace), scaler and IPC algorithm, colour matrix, cropping/paste windows, and per-frame user data.

## Error codes

Errors encode in `0x806104xx` + `0x806181xx` (ENT) + `0x806182xx` (IPC) + `0x806183xx` (VSC) + `0x806184xx` (CSC):

| Range | Module |
|---|---|
| 0x80610410–0x806104c1 | Base vpost |
| 0x80618110–0x806181c2 | Entry module |
| 0x80618210–0x806182c2 | IPC |
| 0x80618310–0x806183c2 | Video scaler |
| 0x80618410–0x806184c2 | CSC |

Key base errors:
| Error | Value |
|---|---|
| `CELL_VPOST_ERROR_Q_ARG_CFG_NULL` | 0x80610410 |
| `CELL_VPOST_ERROR_Q_ARG_CFG_INVALID` | 0x80610411 |
| `CELL_VPOST_ERROR_O_ARG_RSRC_NULL` | 0x80610442 |
| `CELL_VPOST_ERROR_O_ARG_RSRC_INVALID` | 0x80610443 |

## Initialisation lifecycle

1. Load sysmodule: `cellSysmoduleLoadModule(CELL_SYSMODULE_VPOST)`.
2. Configure: fill `CellVpostCfgParam` with input/output dimensions and formats.
3. Query memory: `cellVpostQueryAttr(&cfg, &attr)` → `attr.memSize`.
4. Allocate memory, open: `cellVpostOpen(&cfg, &res, &handle)`.
5. Execute per-frame: `cellVpostExec(handle, &ctrl, &inPicInfo, &outPicInfo)`.
6. Close: `cellVpostClose(handle)`.

## Notable ABI quirks

### 1. PRXPTR audit

| Struct | PRXPTR fields |
|---|---|
| `CellVpostResource` | `memAddr` |
| `CellVpostResourceEx` | `memAddr`, `spurs` |

All other structs are scalar-only (no embedded pointers).

### 2. Width-sensitive integers (size_t)

`CellVpostAttr.memSize`, `CellVpostResource.memSize`, `CellVpostResource.ppuThreadStackSize`, and `CellVpostResourceEx.memSize` are declared as `uint32_t` — the SPRX's 32-bit-pointer ABI expects 4-byte width. `CellVpostHandle` is `uint32_t` (SPRX writes 4-byte EA).

### 3. Opaque handle

`CellVpostHandle` is `uint32_t` — the SPRX writes a 4-byte effective address into `*handle` at `cellVpostOpen` time.

## Sample

`samples/codec/hello-ppu-vpost/` — PPU smoke test: loads CELL_SYSMODULE_VPOST, configures a 1920×1080 YUV→RGBA pipeline, queries memory, opens a vpost instance, closes. RPCS3 HLE returns a config-validation error (0x80610443) which confirms the SPRX processed the caller's config struct correctly — the gate's intent is link-surface verification, and the defined error proves the ABI is working.
