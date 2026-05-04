# libsail (cellSail) — high-level multimedia framework

| Property | Value |
|---|---|
| Umbrella | `<cell/sail.h>` |
| Stub archive | `libsail_stub.a` (nidgen) |
| SPRX module | `cellSail` |
| Entry points | 119 |
| Required sysmodules | `CELL_SYSMODULE_SAIL` (0x001d) |
| Sub-headers | 16 under `<cell/sail/>` |

## Architecture

cellSail is the PS3's high-level multimedia playback framework. It sits atop libdmux, libpamf, libadec, libvdec, and libvpost, providing a unified Player→Source→Adapter→Renderer pipeline:

```
CellSailPlayer
 ├── CellSailDescriptor     (stream metadata)
 ├── CellSailSource          (I/O abstraction — file://, x-cell-fs://)
 │   └── CellSailSourceCheck (diagnostic tool)
 ├── CellSailSoundAdapter    (audio output via cellAudio)
 ├── CellSailGraphicsAdapter (video output via GCM/RSX)
 ├── CellSailRendererAudio   (pull-style audio renderer)
 ├── CellSailRendererVideo   (pull-style video renderer)
 ├── CellSailFeederAudio     (push-style audio feeder from codec)
 ├── CellSailFeederVideo     (push-style video feeder from codec)
 ├── CellSailAuReceiver      (low-level AU pull)
 ├── CellSailProfile         (encoder configuration)
 ├── CellSailMp4Movie        (MP4 format support)
 ├── CellSailAviMovie        (AVI format support)
 └── CellSailVideoConverter  (video format conversion)
```

The Player manages state transitions (Initialized→Opened→Running) via event callbacks. Users supply adapter implementations (SoundAdapter / GraphicsAdapter) and feeder implementations (FeederAudio / FeederVideo) that plug into the pipeline. Sailing (streaming with policy) is the core abstraction — the Player handles demux, decode, AV-sync, trick-mode transitions, and end-of-stream management.

## Surface (per component)

### Memory allocator (root init)

| Function | Signature |
|---|---|
| `cellSailMemAllocatorInitialize` | `int(CellSailMemAllocator*, const CellSailMemAllocatorFuncs*, void*)` |

Must be called first — provides the global allocation callbacks the SPRX uses.

### Player (~35 functions)

| Category | Functions |
|---|---|
| Lifecycle | `cellSailPlayerInitialize`, `cellSailPlayerInitialize2`, `cellSailPlayerFinalize` |
| Boot | `cellSailPlayerBoot` |
| Descriptor/Source | `cellSailPlayerAddDescriptor`, `cellSailPlayerAddSource` |
| Adapters | `cellSailPlayerAddGraphicsAdapter`, `cellSailPlayerAddSoundAdapter` |
| Renderers | `cellSailPlayerAddRendererAudio`, `cellSailPlayerAddRendererVideo` |
| Feeders | `cellSailPlayerAddFeederAudio`, `cellSailPlayerAddFeederVideo` |
| AU receivers | `cellSailPlayerAddAuReceiver` |
| Stream control | `cellSailPlayerOpenStream`, `cellSailPlayerCloseStream` |
| ES control | `cellSailPlayerOpenEs{Audio,Video,User}` / `CloseEs{Audio,Video,User}` / `ReopenEs{Audio,Video,User}` |
| Transport | `cellSailPlayerSetStartOffset`, `cellSailPlayerSetLoop`, `cellSailPlayerSetPause`, `cellSailPlayerSetNext` |
| Parameters | `cellSailPlayerSetParameter`, `cellSailPlayerGetParameter` |
| Query | `cellSailPlayerGetState`, `cellSailPlayerGetElapsedTime`, `cellSailPlayerGetRemainingTime`, `cellSailPlayerGetWholeTime`, `cellSailPlayerGetLastError` |

### Source

| Function | Signature |
|---|---|
| `cellSailSourceInitialize` | `int(CellSailSource*, const CellSailSourceFuncs*, void*)` |
| `cellSailSourceFinalize` | `int(CellSailSource*)` |
| Notifications | `cellSailSourceNotifyCallCompleted`, `...InputEos`, `...StreamOut`, `...SessionError`, `...MediaStateChanged` |
| `cellSailSourceSetDiagHandler` | `CellSailSourceFuncDiagNotify(...)` |

### Source check (diagnostic)

| Function | Signature |
|---|---|
| `cellSailSourceCheck` | `int(CellSailSource*, const CellSailSourceFuncs*, void*, const char*, CellSailSourceCheckResource*, CellSailSourceCheckFuncError, void*)` |

### Sound / Graphics adapters

cellSail owns the playback pipeline; adapters bridge to output hardware (cellAudio / GCM/RSX).

| Adapter | Functions |
|---|---|
| Sound | `Initialize`, `Finalize`, `SetPreferredFormat`, `GetFrame`, `GetFormat`, `UpdateAvSync`, `PtsToTimePosition` |
| Graphics | `Initialize`, `Finalize`, `SetPreferredFormat`, `GetFrame`, `GetFrame2`, `GetFormat`, `UpdateAvSync`, `PtsToTimePosition` |

### Renderers + Feeders

| Component | Functions |
|---|---|
| RendererAudio | `Initialize`, `Finalize`, `NotifyCallCompleted`, `NotifyFrameDone`, `NotifyOutputEos` |
| RendererVideo | (same shape as audio) |
| FeederAudio | `Initialize`, `Finalize`, `NotifyCallCompleted`, `NotifyFrameOut`, `NotifySessionEnd`, `NotifySessionError` |
| FeederVideo | (same shape as audio) |

### AU receivers

| Function | Signature |
|---|---|
| `cellSailAuReceiverInitialize` | `int(CellSailAuReceiver*, void*, void*, int)` |
| `cellSailAuReceiverFinalize` | `int(CellSailAuReceiver*)` |
| `cellSailAuReceiverGet` | `int(CellSailAuReceiver*, uint64_t, uint32_t, uint64_t, const CellSailAuInfo**)` |

### Profiles + format-specific

| Component | Functions |
|---|---|
| Profile | `SetStreamParameter`, `SetEsAudioParameter`, `SetEsVideoParameter` + inline helpers |
| MP4 | Movie/Track queries: `GetBrand`, `GetMovieInfo`, `GetTrackByIndex/Id/Type`, `GetTrackInfo`, `GetTrackReference` |
| AVI | `GetMovieInfo`, `GetStreamByIndex`, `GetStreamHeader` |
| Descriptor | `Open`, `Close`, `GetStreamType`, `GetUri`, `SetEs`, `ClearEs`, `GetCapabilities`, `InquireCapability` |
| Converter | `CanProcess`, `Process`, `CanGetResult`, `GetResult` |

## Error codes

All encode in `0x806107xx`:

| Error | Value |
|---|---|
| `CELL_SAIL_ERROR_INVALID_ARG` | 0x80610701 |
| `CELL_SAIL_ERROR_INVALID_STATE` | 0x80610702 |
| `CELL_SAIL_ERROR_UNSUPPORTED_STREAM` | 0x80610703 |
| `CELL_SAIL_ERROR_EMPTY` | 0x80610705 |
| `CELL_SAIL_ERROR_MEMORY` | 0x806107F0 |
| `CELL_SAIL_ERROR_FATAL` | 0x806107FF |

## Initialisation lifecycle

1. Load sysmodule: `cellSysmoduleLoadModule(CELL_SYSMODULE_SAIL)`.
2. Initialize allocator: `cellSailMemAllocatorInitialize(&alloc, &funcs, arg)` — provides malloc/free equivalents.
3. Create Player: `cellSailPlayerInitialize(&player, eventCb, arg, preset)`.
4. Add descriptors and sources: `cellSailPlayerAddDescriptor`, `cellSailPlayerAddSource`.
5. Add adapters: `cellSailPlayerAddGraphicsAdapter`, `cellSailPlayerAddSoundAdapter`.
6. Boot: `cellSailPlayerBoot`.
7. Open media: `cellSailPlayerOpenStream`.
8. Playback loop: Player manages decode/demux/AV-sync via event callbacks.
9. Tear down: `cellSailPlayerCloseStream` → `cellSailPlayerFinalize`.

## Notable ABI quirks

### 1. PRXPTR-heavy callback tables

Every adapter, renderer, feeder, and source interface carries a function-pointer table (CellSailRendererAudioFuncs, etc.) with 9–14 funcptr fields each. ALL funcptr fields are tagged `ATTRIBUTE_PRXPTR` — without this, the LP64 compiler emits 8-byte function pointers while the SPRX expects 4-byte EAs. Callback functions must be compiled with `-mcpu=cell` so the linker can compute the 4-byte EA of the .opd descriptor.

### 2. SPRX Player cleanup-path crash

`cellSailPlayerInitialize` with incomplete configuration returns `CELL_SAIL_ERROR_INVALID_ARG` but the SPRX's internal cleanup path may dereference uninitialized fields. This is a known RPCS3 behaviour — smoke samples should exercise the allocator surface (not the Player) to avoid RPCS3 freezes.

### 3. Forward-declaration ordering

`CellSailStartCommand` is a typedef alias for `CellSailSourceStartCommand` (defined in source.h). Our `common.h` uses a forward declaration `struct CellSailSourceStartCommand;` to make the alias available before the full struct definition. The umbrella has been reordered (source.h before player.h) so consumers including `<cell/sail.h>` see types in dependency order.

### 4. Shared codec types

Uses `CellCodecTimeStamp` and `CellCodecEsFilterId` from `<cell/codec/types.h>` (shipped with libdmux). `CELL_SAIL_4CC()` macro constructs FourCC codes from ASCII.

## Sample

`samples/codec/hello-ppu-sail/` — PPU smoke test validated in RPCS3: loads CELL_SYSMODULE_SAIL, initializes a memory allocator with real malloc/free callbacks, exits cleanly. Does not exercise the full Player pipeline (out of scope for the smoke gate).
