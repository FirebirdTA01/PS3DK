# Next-session prompt — debug hello-spu-job (JOB CHAIN) regression

## TL;DR

`samples/spurs/hello-spu-job` worked end-to-end at commit `9068e6e`
(2026-04-28 morning).  By the end of the same day's session — after
the JOB QUEUE subsystem (commits `4b0b2e1` → `e6c0c1d` → `20f29c8` →
`56f3dbb` → `a393fd9`) was brought online — it now halts with
`CELL_SPURS_JOB_ERROR_JOB_DESCRIPTOR (0x80410a0b)` even when the
sample is built from the exact `9068e6e` source tree.  JOB QUEUE
(`hello-spurs-jq`) is unaffected and runs perfectly: 6/6 jobs,
multi-SPU dispatch, semaphore-blocked join, exit 0, "SUCCESS".

The previous session's investigation is captured in
`memory/project_jc_regression.md` — read that first.

## What the failure looks like

```
RunJobChain ok; polling for sentinel ...
  [round 0] isHalted=0 statusCode=0 cause=0x0 PC=0x10010d10 LR={0,0,0} grabbed=16
  [round 1] isHalted=0 statusCode=0 cause=0x0 PC=0x10010d18 LR={0,0,0} grabbed=16
  [round 2] isHalted=1 statusCode=0x80410a0b cause=0x10010c00 PC=0x10010d18 LR={0,0,0} grabbed=16
  jc[halted] raw bytes [0x70..0xa0]:
    70: 01 00 00 10 00 00 00 00 00 00 00 00 40 00 16 80
    80: 80 41 0a 0b 00 00 00 00 00 00 00 00 10 01 0c 00
    90: 00 33 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

* `cause = 0x10010c00` is the heap-allocated `CellSpursJobChain *jc`.
* `PC = 0x10010d18` is a PRX-mode pointer inside `libspurs.sprx`'s
  `cellSpursRunJobChain` spin-loop.
* `isHalted` flips from 0 → 1 between rounds 1 and 2, so the
  dispatcher accepts the chain *initially*, then trips a deferred
  validation.  The SPU side never gets control.

## What was already ruled out

(All probed in the prior session — don't redo.)

* **Layout mismatch.**  The SPU image has the right shape:
  `.SpuGUID 0..0xf, .before_text 0x10..0x2f, .text 0x30+, .data
  0x200, .note.spu_name not loaded`.  `e_entry = 0x10`, `e_flags = 1`.
* **Descriptor field bytes.**  `eaBin = 0x10000580` (16-byte aligned),
  `sizeBin16 = 33` (= 528 bytes), `jobType = 4`, `userData[0]/[1]`
  populated, all other fields zero — byte-identical to the working
  build.
* **PPU JIT cache** (`~/.cache/rpcs3/cache/ppu-*hello-spu-job*`) — moved
  aside, no change.  Cache lookup uses SELF SHA1 → fresh translation
  on every rebuild anyway.
* **dev_flash overlays** — moved `libsre.prx`, `libspurs_jq.prx`,
  `libaacenc_spurs.prx` aside (these are decrypted-PRX siblings of
  the encrypted `.sprx`).  Per `feedback`-noted RPCS3 behaviour, the
  emulator ignores `.prx` and uses `.sprx` at runtime — confirmed,
  removing them changed nothing.
* **--start-group/--end-group on single-lib LIBS** — gated to
  multi-lib only in `cmake/ps3-self.cmake`, no change.
* **Source regressions** — built the `9068e6e` tree directly via
  `git checkout 9068e6e` then full rebuild → same failure.

## What to try

### 1. Get the dispatcher's view via SPRX disassembly

The PC at halt (`0x10010d18`) is a PRX-mode offset into
`libspurs.sprx`.  Decrypt and disassemble:

```bash
/usr/bin/rpcs3 --decrypt ~/.config/rpcs3/dev_flash/sys/external/libspurs.sprx
# Writes ~/.config/rpcs3/dev_flash/sys/external/libspurs.elf next to it
powerpc64-ps3-elf-objdump -d ~/.config/rpcs3/dev_flash/sys/external/libspurs.elf | less
```

Find the function whose body contains offset `0x10d18` (relative to
the SPRX's load base).  Look for: `lis 0x8041 + ori 0xa0b` building
the error code, or a `stw` that writes `0x80410a0b` to `jc + 0x80`.
Trace the decision-making conditional that leads there.  Likely
candidates per the prior session's intuition:

* `numSpus` mismatch (we pass 1, dispatcher might want a different
  value)
* command-list size validation (`s_chain[]`)
* workload-table bookkeeping in libspurs's job-chain registry
* `cellSpursJobChainAttributeSetHaltOnError(true)` is what's setting
  `isHalted=1`; without it the chain might keep running

### 2. Run with HaltOnError off

Quick test — drop `cellSpursJobChainAttributeSetHaltOnError(&jcAttr)`
from the sample.  If `isHalted=0` the entire run, the SPU might
actually run despite the spurious 0xa0b.  Tells us whether the halt
itself is symptom or cause.

### 3. RPCS3 GDB stub watchpoint on `&jc.error`

```yaml
# ~/.config/rpcs3/config.yml — enable GDB stub:
Core:
    GDB Server:
        Enable: true
        Port: 2345
```

Run RPCS3, attach gdb to TCP 2345, set hardware watchpoint on
`<jc>+0x80` (= where `0x80410a0b` lands).  When it triggers, the
backtrace points at the writer.

### 4. Compare libspurs.sprx call-graph for JC vs JQ

JOB QUEUE works from the same `libspurs.sprx`.  Diff the JC and JQ
code paths in libspurs to identify what JC does differently that
might trigger the deferred validation.  JQ uses
`cellSpursCreateJobQueue → cellSpursJobQueuePushJob`; JC uses
`cellSpursCreateJobChainWithAttribute → cellSpursRunJobChain`.

### 5. Sanity-check our JOB CHAIN side hasn't drifted

The ABI doc `docs/abi/spurs-job-entry-point.md` describes the JC
contract (current as of the JOB QUEUE work).  Re-read it against the
current `samples/spurs/hello-spu-job/` layout for any subtle
mismatch the regression analysis missed.

## Building reference-SDK samples for comparison

The reference SDK toolchain lives under
`~/cell-sdk/3.70-extract/toolchain/cell/host-win32/` (extracted from
`~/cell-sdk/PS3 SDK 3.70 + PhyreEngine.7z`, payload
`InstallFiles/[36]/PS3_Toolchain_411-Win_370_001.zip`).  The 4.75
toolchain is on a different machine and not yet copied; 3.70's
output is byte-equivalent for the entry-point ABI.

### SPU compile + link

```bash
cd /tmp/ref_jc
WINE_BIN=~/cell-sdk/3.70-extract/toolchain/cell/host-win32/spu/bin/spu-lv2-gcc.exe
SDK_INC="-I$(winepath -w ~/cell-sdk/3.70-extract/sdk-runtime/cell/target/spu/include) \
         -I$(winepath -w ~/cell-sdk/3.70-extract/sdk-runtime/cell/target/common/include)"
SDK_LIB="-L$(winepath -w ~/cell-sdk/3.70-extract/toolchain/cell/host-win32/spu/lib/gcc/spu-lv2/4.1.1/pic) \
         -L$(winepath -w ~/cell-sdk/3.70-extract/sdk-runtime/cell/target/spu/lib/pic)"

wine "$WINE_BIN" -fpic -mspurs-job -Wl,-q -O2 \
    $SDK_INC $SDK_LIB ref_job_hello.cpp -o ref_job_hello.elf
```

The reference linker spec wants `job_start.o` and friends from
`spu/lib/gcc/spu-lv2/4.1.1/pic/`; either put a copy under the
working dir or pre-stage them.  `-mspurs-job` → simple `_start`,
`-mspurs-job-initialize` → with-CRT `_start` + `__job_start` chain.

### Wrapping into jobbin2 + extracting jobheader

```bash
WINEPATH="$(winepath -w ~/cell-sdk/3.70-extract/toolchain/cell/host-win32/ppu/bin)" \
    wine ~/cell-sdk/475.001/cell/host-win32/bin/spu_elf-to-ppu_obj.exe \
    --format=jobbin2 --objcopy-style-symbol \
    ref_job_hello.elf ref_job_hello.ppu.o
# Outputs:
#   ref_job_hello.ppu.o     - PPU obj with .spu_image + .spu_image.jobheader
#                             sections, but with PPU GCC 4.1's e_flags
#                             that our binutils 2.42 rejects when linking
#   ref_job_hello.jobbin2   - the raw blob (= 0x100 ELF prefix + LS image)
```

This is what the `JOBBIN_WRAP` flag in `cmake/ps3-self.cmake`
(commit `e6c0c1d` onward) drives at build time.  The PPU `.ppu.o`'s
`.spu_image.jobheader` section is extracted via PPU `objcopy
--output-target=binary --only-section=.spu_image.jobheader` to
sidestep the e_flags incompatibility.

## RPCS3 quirks to keep in mind

* **`/usr/bin/rpcs3` is the system-installed binary.**  Source-clone
  builds (`~/source/repos/RPCS3/build/bin/rpcs3`) are separate and
  may have stale debug patches; always test against `/usr/bin/rpcs3`
  unless the patch is intentionally part of the test.
* **`rpcs3 --no-gui <path/to/.self>` runs headless and exits when the
  PPU process calls `sys_process_exit()`.**  Without that exit, the
  emulator window stays up and the user has to PS-button out.
  Always build a force-exit path on timeout into samples — see
  pattern in `samples/spurs/hello-spurs-jq/source/main.cpp`.
* **`.sprx` vs `.prx` in dev_flash.**  `.sprx` is the encrypted/signed
  PRX RPCS3 actually loads.  `.prx` siblings are decrypted dumps
  (typically from `rpcs3 --decrypt`) — RPCS3 ignores them at runtime
  unless explicitly configured.  Patches to `.prx` files have NO
  runtime effect; don't waste time editing them as a "patch the
  dispatcher" approach.  To experiment with dispatcher behaviour
  changes, build RPCS3 from source instead.
* **PPU JIT cache:** `~/.cache/rpcs3/cache/ppu-<sha1>-<name>.self/`.
  Indexed by SELF SHA1 — every rebuild produces a fresh entry, so
  cache invalidation isn't needed across rebuilds.  Old entries
  accumulate; they're harmless but big.  Never RM the cache;
  `mv` aside if you need a clean slate.
* **SPU programs cache:** `~/.cache/rpcs3/spu_progs/` and the
  `<self-cache>/v7-kusa-*-znver4.obj.gz` files inside per-SELF
  cache dirs.  Compiled SPU translations.
* **TTY + RPCS3 logs:** `~/.cache/rpcs3/TTY.log` is `printf` output;
  `RPCS3.log` is RPCS3's own diagnostic stream.  Both are
  overwritten on each launch — `mv` aside before each test if you
  want to keep them.

## Sample-side timeout-exit pattern

Always include this in any sample/test that talks to SPURS, so the
emulator closes cleanly when the test fails:

```cpp
#include <sys/process.h>

// ... after polling/wait loop ...
if (jq_halted || !got_sentinel) {
    std::printf("FAILURE: bypassing teardown to force exit\n");
    spu_printf_finalize();
    cellSysmoduleUnloadModule(CELL_SYSMODULE_SPURS_JQ);
    sys_process_exit(1);
}
```

The `Shutdown/Join` path can hang indefinitely waiting on a stuck
SPU kernel after a halt — `sys_process_exit` bypasses it.  The
`hello-spu-job` JOB CHAIN sample also has a similar guard around its
`cellSpursShutdownJobChain → cellSpursJoinJobChain` pair.

## House rules to honour

(See memory entries `feedback_*`.)

* No vendor names ("Sony", "SCE") in tracked content — say "reference
  SDK" or describe the binary contract directly.
* No external-project refs in source/commits (no naming downstream
  consumers like specific games or engines).
* RE narrative goes to `memory/` or untracked `docs/` — *not* into
  tracked `docs/abi/`.  Tracked ABI docs describe the contract
  directly, no archaeology.
* Don't commit without explicit user confirmation; stage and ask.

## Files to read first

* `memory/project_jc_regression.md` — full prior-session details
* `memory/project_spurs_job_runtime.md` — the JC working state
  (descriptor / layout / etc.)
* `memory/project_spurs_jq_runtime.md` — the JQ contrast (works
  fine; useful for diff)
* `samples/spurs/hello-spu-job/source/main.cpp` (PPU)
* `samples/spurs/hello-spu-job/spu/source/main.c` (SPU body)
* `sdk/libspurs_job/src/job_start.S` (SPU entry trampoline)
* `sdk/libspurs_job/scripts/spurs_job.ld` (SPU layout)
* `cmake/ps3-self.cmake` (`ps3_add_spu_image` macro: `JOBBIN` and
  `JOBBIN_WRAP` flag handling)
* `docs/abi/spurs-job-entry-point.md` (the public ABI doc)

## Last commit to anchor against

```
a393fd9 samples + sdk: timeout-exit polish, --start-group only for multi-lib
56f3dbb samples: hello-spurs-jq drives sync via cellSpursJobQueueSemaphore
20f29c8 samples: hello-spurs-jq pushes 6 jobs through the queue, all complete
e6c0c1d sdk: hello-spurs-jq runs end-to-end; with-CRT _start + JOBBIN_WRAP build
4732528 sdk: libspurs_jq cellSpursJobMain2 wrapper + cross-archive link fix
4b0b2e1 sdk: libspurs_jq SPU runtime skeleton (stubs for all 53 entry points)
a0f4bdd sdk: cell/spurs/job_queue*.h surface (PPU + SPU headers; runtime stubs only)
9068e6e sdk: hello-spu-job runs end-to-end; correct BINARY2 descriptor + flat-image embed  ← LAST KNOWN GOOD JC
```
