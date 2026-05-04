# libsail_rec (cellSailRec) — SAIL recording/muxing framework

| Property | Value |
|---|---|
| Headers | `<cell/sail_rec.h>`, `<cell/sail/common_rec.h>`, `<cell/sail/muxer.h>`, `<cell/sail/recorder.h>` |
| Stub archive | `libsail_rec_stub.a` (nidgen) |
| SPRX module | `cellSailRec` |
| Entry points | 58 (39 in 3.70, +19 Composer in 475) |
| Required sysmodules | `CELL_SYSMODULE_SAIL_REC` (0xf034) |
| Co-resident | Same include directory as libsail (`<cell/sail/*.h>`) |

## Surface

libsail_rec is the recording/muxing counterpart to libsail's playback framework. It provides:
- **Composer** — low-level on-the-fly multiplexer (475+ surface)
- **Recorder** — full state-machine encoder lifecycle

### Composer (~19 functions, 475+ only)

| Function | Signature |
|---|---|
| `cellSailComposerInitialize` | `int(CellSailComposer*, CellSailMp4MovieParameter*, CellSailStreamParameter*, unsigned)` |
| `cellSailComposerFinalize` | `int(CellSailComposer*)` |
| `cellSailComposerGetStreamParameter` | `int(CellSailComposer*, unsigned, CellSailStreamParameter*, void**)` |
| `cellSailComposerSetEsAudioParameter` | `int(CellSailComposer*, unsigned, const CellSailComposerEsAudioParameter*)` |
| `cellSailComposerSetEsVideoParameter` | `int(CellSailComposer*, unsigned, const CellSailComposerEsVideoParameter*)` |
| `cellSailComposerSetEsUserParameter` | `int(CellSailComposer*, unsigned, CellSailStreamParameter*, void*)` |
| `cellSailComposerGetEsAudioAu` | `int(CellSailComposer*, unsigned, CellSailComposerAudioAuInfo*, CellSailCommand*)` |
| `cellSailComposerTryGetEsAudioAu` | (non-blocking variant) |
| `cellSailComposerGetEsVideoAu` | `int(CellSailComposer*, unsigned, CellSailComposerVideoAuInfo*, CellSailCommand*)` |
| `cellSailComposerTryGetEsVideoAu` | (non-blocking variant) |
| `cellSailComposerGetEsUserAu` | `int(CellSailComposer*, unsigned, CellSailComposerUserAuInfo*, CellSailCommand*)` |
| `cellSailComposerTryGetEsUserAu` | (non-blocking variant) |
| `cellSailComposerGetEsAudioParameter` | `int(CellSailComposer*, unsigned, CellSailComposerEsAudioParameter*)` |
| `cellSailComposerGetEsVideoParameter` | `int(CellSailComposer*, unsigned, CellSailComposerEsVideoParameter*)` |
| `cellSailComposerGetEsUserParameter` | `int(CellSailComposer*, unsigned, CellSailStreamParameter*, void**)` |
| `cellSailComposerRegisterEncoderCallbacks` | `int(CellSailComposer*, unsigned, void*)` |
| `cellSailComposerUnregisterEncoderCallbacks` | `int(CellSailComposer*, unsigned)` |
| `cellSailComposerNotifyCallCompleted` | `int(CellSailComposer*, int)` |

### Recorder (~27 functions)

Mirrors the Player's lifecycle: Initialize / Finalize / Boot / OpenStream / CloseStream / OpenEs{Audio,Video,User} / CloseEs{Audio,Video,User} / SetStartOffset / SetPause / SetParameter / GetParameter / GetState / GetElapsedTime / GetRemainingTime / GetLastError / AddDescriptor / AddSource / AddFeeder{Audio,Video} / GetCaptureFrameInfo.

### Common types

`CellSailCommand`, `CellSailFeederFrameInfo` (with PRXPTR on `pFrame` and `pCommands` fields), `CellSailCaptureFrameInfo`.

## Error codes

Shared with libsail (`0x806107xx`). Composers return `CELL_SAIL_ERROR_INVALID_ARG` on incomplete configuration.

## Notable ABI quirks

### 1. SDK-version delta (largest in the project)

3.70 firmware ships 39 entry points. 475 adds 19 cellSailComposer* functions — a real-time audio+video composition / multiplexer surface. Downstream consumers targeting pre-475 firmware should guard the Composer entries with a runtime-version check. The smoke sample exercises the Composer surface specifically because it's simpler and avoids the Recorder's deep state machine.

### 2. Recorder vs Composer

The Recorder is the full encoder lifecycle: open file → encode → close file. The Composer is a lower-level on-the-fly multiplexer that lets callers push encoded AUs directly without a file container. The smoke test uses ComposerInitialize / ComposerFinalize to avoid the Recorder's state machine (which has the same SPRX-cleanup crash risk as the Player path in libsail).

### 3. Header dependency graph

The recorder depends on most of libsail's sub-headers: `source.h`, `descriptor.h`, `feeder_audio.h`, `feeder_video.h`. Our `recorder.h` includes these directly. The `muxer.h` (Composer) includes `profile.h` for `CellSailMp4MovieParameter` and `CellSailStreamParameter`. Consumers should just include the umbrella `<cell/sail_rec.h>` which chains through all dependencies.

### 4. PRXPTR audit

| Struct | PRXPTR fields |
|---|---|
| `CellSailComposerEsAudioParameter` | `pSpecificInfo` |
| `CellSailComposerEsVideoParameter` | `pSpecificInfo` |
| `CellSailComposer{Audio,Video,User}AuInfo` | `pAu`, `specificInfo` |
| `CellSailFeederFrameInfo` | `pFrame`, `pCommands` |

### 5. Co-resident with libsail

Both libsail and libsail_rec share the `sdk/include/cell/sail/` directory tree. They are separate SPRX modules with separate stub archives. cmake link lines for recording apps need both `-lsail_stub` and `-lsail_rec_stub`.

## Sample

`samples/codec/hello-ppu-sail-rec/` — PPU smoke test validated in RPCS3: loads CELL_SYSMODULE_SAIL_REC, initializes a Composer with MP4 parameters, finalizes cleanly. Composer calls return defined error codes (0x80610701/0x80610702) — expected for an under-configured pipeline; proves link/ABI surface works.
