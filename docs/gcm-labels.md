# RSX labels, reports and main-memory arenas

This page states the label and report reservation rule, the non-blocking completion query, what a completion
label guarantees before a buffer is reused, and the rules for drawing from a `cellGcmMapMainMemory` arena. Each
statement says how it is verified. "RPCS3" means one run of the named sample on the desktop RPCS3 build.
"Console" means a run of the hooked SELF on a retail PS3 through the loader, as reported back from that run.

The API is in `<cell/gcm/ps3tc_gcm_labels.h>`. `<cell/gcm.h>` includes it, so the cell-style names below are
available wherever the GCM headers are. These are canonical cell-style names. PSL1GHT has no equivalent, so no
`rsx*` alias exists.

## 1. Reservation rule

| Labels (0..255) | Owner | Who writes them |
|---|---|---|
| 0..63 | SYSTEM | firmware and GCM system use; never used by application code (`CELL_GCM_INDEX_RANGE_LABEL_MIN` is 64) |
| 65 | PSGL | libPSGL prepared-buffer label |
| 66..68 | PSGL | libPSGL buffer-status labels, one per PSGL frame buffer (`PSGL_MAX_FRAME_BUFFERS` = 3) |
| 240..247 | PROFILER | the block profiler samples lend to `ps3dk_prof_gpu_attach` and use for their fences; the automatic profiler's default labels (246 clock anchor, 247 operation) |
| 255 | PSGL | libPSGL idle/finish token |
| 64, 69..239, 248..254 | APPLICATION | yours. Use `CELL_GCM_LABEL_APP_FIRST..CELL_GCM_LABEL_APP_LAST` (69..239) for per-slot frame labels |

| Local reports (0..2047) | Owner |
|---|---|
| 0..511 | PROFILER: profiler samples' timestamp reports and the GPU-domain report ranges they lend |
| 512..1023 | APPLICATION (`CELL_GCM_REPORT_APP_FIRST..CELL_GCM_REPORT_APP_LAST`) |
| 1024..2047 | PROFILER: the automatic profiler's default report pool (`PS3_PROF_AUTO`) |

Reports 1024..2047 were the application's before beta 4; they are now reserved for the profiler, and applications use
512..1023.

The main-memory report area belongs to the application.

What the SDK and profiler use today (inventory of `origin/main` c22cb7ab, rechecked by the test below):

- **SDK libraries.** Only libPSGL uses fixed labels: 65, 66..68 and 255. libgcm_cmd, librsx, libgcm_sys_legacy and
  the profiler library hard-code no label or report index. The GCM headers only forward a caller's index.
- **libps3dkprof.** The manual library owns no fixed index. `ps3dk_prof_gpu_attach(ctx, report_first, report_count,
  label, ...)` borrows an application-lent report range and one label, exclusively. It accepts labels in 64..255 and refuses
  libPSGL's 65..68 and 255 (`gpu_port_gcm.c`).
- **Profiler samples.** heterogeneous-workload uses label 240 (scene fence), 241 (GPU domain), 242 (application
  fence), timestamp reports 0..1 and domain reports 32..63. overhead-bench uses label 241 (domain), 243 (bench),
  reports 2..3 (bench) and 32..287 (domain). clock-calibration and clock-calibration-w use label 241 and report 0.
  ext-probe uses reports 0..15 and label 64. Label 64 is an application label, recorded as a named exception.
- **Automatic profiler (`PS3_PROF_AUTO`).** By default it takes reports 1024..2047, operation label 247 and clock
  label 246 (`PS3DK_PROF_AUTO_OPTIONS_INIT` in `<ps3dk/prof_auto.h>`), all PROFILER-owned. The attach refuses
  defaults that would take an application index. An application override (`ps3dk_prof_auto_attach` with options)
  chooses its own indices and must reserve them itself.
- **Not found:** no code in this repository uses reports 512..528. If a design document lists them, they are free
  under this rule: they lie in the application range.

Query:

```c
uint32_t n;
const CellGcmIndexRange *r = cellGcmGetReservedLabelRanges(&n);  /* ascending, disjoint */
const CellGcmIndexRange *q = cellGcmGetReservedReportRanges(&n);
uint32_t owner = cellGcmGetLabelOwner(index);   /* 0 = application, else CELL_GCM_INDEX_OWNER_* */
uint32_t rowner = cellGcmGetReportOwner(index);
```

Verified by `tests/sdk/gcm-labels-test.sh`, a host test that CI runs:

- **Tables.** It checks the tables (owners, edges, disjointness) and builds the header in C and in C++17.
- **Source scan.** `tests/sdk/gcm-label-reservation-test.py` finds every label and report API call in `sdk/`,
  `samples/profiling/` and the two samples below, evaluates each index from the code's own macros, and fails if an
  index is outside the declared set for that tree:
  - the SDK may use PSGL- or PROFILER-owned indices only;
  - profiler samples may use PROFILER-owned indices, plus the listed exceptions;
  - the application samples may use application indices only.
- **Non-constant indices.** An index that is not a constant fails unless its file is an API forwarder, or the test
  declares the values it can take.
- **Drift checks.** Three checks pin libPSGL's label macros, the profiler port's refused set and the automatic
  profiler's default labels and report pool to the header.
- **Self-test.** `--self-test` shows that the scan refuses each kind of violation.

## 2. Non-blocking completion query (A4)

```c
int passed = cellGcmIsLabelPassed(index, serial);   /* never waits, never writes */
```

- **Read.** It reads the label once with `cellGcmReadLabelOrdered`, a volatile load followed by `sync`, and
  compares in 32-bit serial arithmetic: `cellGcmIsSerialReached(current, target)` is `(uint32_t)(current - target)
  < 2^31`.
- **Wrap-around.** A serial that wraps past 0xffffffff still compares correctly while the label and the target are
  less than 2^31 apart. At 2^31 or more the answer is the wrap-around one and has no meaning. With one increment
  per frame this limit is never reached.
- **Host test.** The test covers the wrap boundary: 0 has reached 0xffffffff, 0xffffffff has not reached 0, the
  2^31 edge, and a polled label stepping across 0xfffffff0..0x2f. A control built with a plain `>=` must fail
  exactly those six cases, and it does.
- **Why `sync`.** The label area is RSX-written memory that the PPU reads uncached. `lwsync` does not order
  caching-inhibited accesses, so `sync` is used. After it, a later PPU store (for example into an arena slot)
  cannot be performed ahead of the label read. This is an argument from the PowerPC storage model. No test
  measures it.
- **Byte order.** `cellGcmSetWriteBackEndLabel` byte-swaps the value it emits, and the back end writes it back in
  PPU order. The PPU therefore reads back the value that was passed, which the RPCS3 runs below confirm.

## 3. What a completion label guarantees (A2)

Sample: `samples/gcm/hello-ppu-gcm-label-reuse`.

**Set-up.** Each trial makes 64 full-target draws (256x256) that read their vertices, their indices and one
texture from a mapped main-memory arena. The draws are followed by the completion label of the mode under test
(application label 101). As soon as the PPU sees that label pass, it overwrites the arena: the vertices first,
moved off the target, then the texture with a POISON colour.

**Check.** An image transfer copies the target to main memory, and a back-end label (102) completes that copy.
The PPU then checks the copy:

- POISON means a late texture read;
- the clear colour means a late vertex or index fetch;
- a sentinel means the copy never landed.

**Warm-up.** RPCS3 compiles shaders asynchronously, so warm-up trials without poison run first, until one renders.

| Mode | Label | RPCS3 (32 trials) | Console (32 trials) |
|---|---|---|---|
| backend | `cellGcmSetWriteBackEndLabel` after the last draw | 0 corrupt | 0 corrupt |
| texture | `cellGcmSetWriteTextureLabel` after the last draw | 0 corrupt | 0 corrupt |
| command (control) | `cellGcmSetWriteCommandLabel` after the last draw | 0 corrupt | 0 corrupt |
| early (control) | `cellGcmSetWriteCommandLabel` before the first draw | 32 corrupt, every one a late vertex fetch | 32 corrupt (676 bad samples each) |

**Console result: A2 INCONCLUSIVE.** The signed SELF exited 0 with the detector sensitive (the early control is
caught). But the command-label control is clean on silicon too, so this workload finishes too fast to separate a
command label from a back-end label, and the clean back-end and texture rows prove nothing yet.

**Heavy variant.** `samples/gcm/hello-ppu-gcm-label-reuse-heavy` builds the same source with `LABEL_REUSE_HEAVY`:

- a 1024x1024 texture (4 MiB of arena);
- 256 draws over a 512x512 target;
- a fragment program that makes eight cache-missing texture fetches per pixel and combines them with `max()`, which
  is exact at any precision.

The aim is to keep the RSX reading the arena long after its front end passes a command label. It prints
`GCM_LABEL_REUSE_A2 conclusive` only when the command control is caught while backend and texture stay clean. Every
wait is bounded. Each line is also appended to `/dev_hdd0/tmp/gcmlabel-crumb.txt` and fsync'd.

**RPCS3, heavy:**

| Mode | Corrupt trials |
|---|---|
| backend | 0/32 |
| texture | 0/32 |
| command (control) | 0/32 |
| early (control) | 32/32 |

The verdict is `GCM_LABEL_REUSE_A2 inconclusive`. RPCS3 cannot show the command-label hazard at any workload size:
its RSX thread reads a draw's vertices and textures when it executes the draw, in FIFO order, so a later command
label is always written after those reads. Only the console run of the heavy variant can make A2 conclusive.

**Console, heavy** (signed SELF on a PS3, 2026-11-10, evidence `hw-s-label-heavy`): exit 0; backend 0/32, texture
0/32, command (control) 0/32, early (control) 32/32; `GCM_LABEL_REUSE_A2 inconclusive (the command control was not
caught)`. **A2 remains INCONCLUSIVE on silicon:** even this workload did not expose the command-label hazard, so the
clean backend and texture results do not yet show those labels are late enough.

What this shows, and what it does not:

- **The detector works on RPCS3.** The early control is caught in every trial, so a label that the emulator
  makes visible before the draws read the arena is detected.
- **RPCS3 does not model the hardware hazard.** The command-label control, which on hardware is written when the
  front end reaches it, ahead of the draws still in the pipeline, is clean on RPCS3. The emulator runs draws in
  FIFO order and has finished their memory reads by the time it reaches a later label. A clean back-end or
  texture result on RPCS3 is therefore NOT evidence for the hardware rule.
- **The texture-only hazard was not seen separately.** The vertices are poisoned before the texture, so a late
  draw shows the clear colour first. No late texture read appeared in any mode.
- **The rule to use, pending hardware.** Write `cellGcmSetWriteBackEndLabel` after a frame's last draw, and reuse
  that frame's vertex, index and texture memory only after `cellGcmIsLabelPassed` reports it. The back-end label is
  released after the frame's rendering reaches the back end, which follows every earlier vertex fetch and texture
  read. `cellGcmSetWriteTextureLabel` is released when earlier texture reads are done. Use it only to reuse
  textures early, never for vertex or index buffers. Both statements are UNVERIFIED on hardware. The heavy variant
  is the hardware test: the expected result is backend = texture = 0 with the command control corrupt. The light
  sample's clean command control on hardware (above) means its draws were too light to expose the window.
- **Not in scope.** The sample does not cover command segments (CALL/RETURN).

## 4. Drawing from a main-memory arena (A3)

Sample: `samples/gcm/hello-ppu-gcm-main-arena`. It draws a textured, indexed quad whose vertices, 16-bit indices
and linear A8R8G8B8 texture all sit in a mapped arena (`CELL_GCM_LOCATION_MAIN`).

**Phases.** It runs three checked phases of 60 frames each:

1. colour A;
2. the PPU rewrites the texture to colour B;
3. the PPU rewrites the vertices so that the quad covers only the left half of the window.

**Check.** Every frame, a 128x128 window at the screen centre is copied to the arena and compared at 50 points.
The frame completes on application label 100.

**RPCS3 result.** `GCM_ARENA_OK frames=180`: a few warm-up frames, then every checked frame correct in all three
phases. A later run (r-arena-3) captured a screenshot showing the colour-A quad on screen.

**Console result: A3 PASS.** The main-arena SELF drew all 180 checked frames on silicon with `GCM_ARENA_OK`.

Rules:

| Rule | Status |
|---|---|
| Map with `cellGcmMapMainMemory(ea, size, &offset)`: `ea` 1 MiB aligned, `size` a nonzero multiple of 1 MiB. Take the memory from LV2 1 MiB pages (`gcm_host_buffer_alloc`), not the sbrk heap | Mapping verified on RPCS3. A request 4 KiB off alignment with a 512 KiB size was refused on RPCS3 (`0x802100ff`). The two conditions were not tested separately |
| Texture offsets 128-byte aligned. The samples also keep vertex and index arrays 128-byte aligned (16 bytes is the vertex-fetch minimum) | Followed by the sample. Misalignment is not tested |
| **PPU write ordering.** Write the data with ordinary stores, then publish the commands that read it with `cellGcmFlush`, which executes `sync` before it stores the PUT pointer (librsx `FlushBuffer`). No extra barrier and no `dcbst`/`dcbf` is needed for data written before that flush | Code inspection plus RPCS3: phases 2 and 3 read the rewritten data. RPCS3 does not model PPU store ordering or caches, so the ordering is UNVERIFIED on hardware |
| If the PPU instead releases already-queued commands by writing a label the RSX waits on (`cellGcmSetWaitLabel`), execute `sync` between the data stores and that label store | Architectural argument only. No sample covers it |
| Rewriting content the RSX has already read, at the same offsets: first wait for the frame's completion label, then emit `cellGcmSetInvalidateTextureCache` / `cellGcmSetInvalidateVertexCache` before the draws that read the new content | RPCS3: phases 2 and 3. Whether the invalidation is required on hardware is UNVERIFIED (RPCS3 detects writes by page protection) |

## Reproducing

```
bash tests/sdk/gcm-labels-test.sh            # host: tables, wrap-around, source scan
```

Both samples print one verdict line: `GCM_ARENA_OK` / `GCM_ARENA_FAIL ...`, and `GCM_LABEL_REUSE_OK` /
`GCM_LABEL_REUSE_FAIL ...` followed by `GCM_LABEL_REUSE_DETECTOR sensitive|not-sensitive`. They build like the
other CMake samples and run under RPCS3 or the loader.
