# EMP engine NID coverage report — 2026-04-26

ELF: `out/build/PS3-Debug/Debug/PS3TestApp/PS3TestApp.elf`
Tool: `/tmp/emp-coverage/coverage.py` (parses `.lib.stub` + `.rodata.sceFNID` + `.rodata.sceResident`, looks up FNIDs against `tools/nidgen/nids/extracted/*.yaml`).

## Headline

**All 218 NIDs the engine imports resolve to known Sony names. 100% NID DB coverage.**

## Imported libraries

| Library         | Imports |
| --------------- | ------: |
| cellGcmSys      |      86 |
| sys_io          |      47 |
| cellSysutil     |       9 |
| cellSysmodule   |       8 |
| sys_fs          |      59 |
| sysPrxForUser   |       9 |

## What the engine is *not* importing

| Function family                     | Status                                                              |
| ----------------------------------- | ------------------------------------------------------------------- |
| `cellSysutilCheckCallback`          | NID known; engine doesn't import it. Menu-nav stutter remains ours. |
| `cellSysutilRegisterCallback`       | NID known; engine doesn't import it.                                |
| `cellGcmSetVertexProgram`           | Implemented in `gcm_cg_bridge.h`; engine uses `rsxLoadVertexProgram`. |
| `cellGcmSetFragmentProgram`         | Implemented in `gcm_cg_bridge.h`; engine uses `rsxLoadFragmentProgramLocation`. |
| `cellGcmCgInitProgram` / `cellGcmCgGetNamedParameter` | Implemented in `libgcm_cmd.a`; engine doesn't link `-lgcm_cmd`. |

## Implications for current debugging

1. The renderer's setDrawEnv FIFO desync **cannot** be a missing-NID problem — every dispatch the engine makes already resolves to a real firmware function via the LV2 loader.
2. Adding sysutil DRAWING_BEGIN/END dispatch is orthogonal to the desync and won't fix it. Still worth doing for menu-nav hygiene, but won't unblock rendering.
3. The cellGcm program-handle path is already shipped end-to-end. Re-pointing the engine at it is purely a downstream code change (link `-lgcm_cmd`, switch `rsxLoad*` to `cellGcmSet{Vertex,Fragment}Program`, switch param setters to `cellGcmCgGetNamedParameter` + `cellGcmSetVertexProgramParameter`).

## Sony-name forwarder gap

`libsysutil.a` exports `__sysUtilCheckCallback` (PSL1GHT-name) but not `cellSysutilCheckCallback` (Sony-name) — even though both share NID `0xa53d12ae`. Engines preferring the Sony surface either need the alias added or the archive re-emitted via nidgen.

Same applies to the broader callback family. Tracked under existing PSL1GHT-decoupling work.

## Recommendation for step 5 follow-up

No NID/stub *implementation* gaps need closing for this engine. The remaining toolchain hygiene items are:

- Add Sony-name aliases to libsysutil for the callback family (small).
- Re-emit libsysutil with nidgen archive flow (drops legacy PSL1GHT names entirely; existing pattern from libsysutil_screenshot, Phase 6.5).
- Verify `libgcm_cmd` is in the canonical sample link line so downstreams that copy it see the program-handle path turned on by default.
