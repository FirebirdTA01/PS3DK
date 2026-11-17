# Explicit zones in SPURS job-queue jobs

This finite source sample submits six jobs, each with a fresh profiler lane.
The job body emits two nested zones and three marks, then flushes and finalizes
its writer before publishing a lane-bound receipt and returning to SPURS.
There are no scheduler hooks. The sample compiles and links in ILP32 ON/OFF
with one enabled SPU, ILP32 ON with two, and LP64 ON with one. **It has not
run on a target; scheduler compatibility and timing remain unmeasured.**

The reference setup is `tests/regression/spurs-suite/{ppu,spu}/job_queue.*`.
It uses four SPURS SPUs in an exclusive non-context group, with one or two
queue priorities enabled by `SJ_ACTIVE_SPUS`. Two enabled SPUs do not prove
actual overlap. The OFF build runs the same checksum work and raw decrementer
observations without creating a lane or attaching a writer.

## Build inputs

Configure this directory using the project's PPU toolchain file. Required
inputs are `SJ_BUILD_ID` (64 lowercase hex digits identifying the pinned build)
and `SJ_EXT_ARCHIVE` (the matching PPU ABI's reviewed extension archive).
`SJ_PROFILING=ON|OFF` selects SPU instrumentation; `SJ_ACTIVE_SPUS=1|2`
selects the queue configuration. `SJ_PROF_SRC` and `SJ_JOB_LDSCRIPT` default
to this repository's reviewed source, not an installed replacement.

The source depends on the documented expansion of `prof_spu.h`'s lane-end
precondition to permit a successful writer fini plus application-proven job
completion and its lane-bound post-fini receipt. Merely compiling against an
older group-join-only contract does not satisfy that prerequisite. This sample
does not claim that `cellSpursFinalize` implements a thread-group join.

CMake builds the job with `-mspurs-job-initialize`, PIC producer sources and
`-Wl,-q`. It generates a sample-local linker script from `spurs_job.ld`, adding
an explicit retained descriptor section before `.fixup`. No SDK script is
edited or installed. The generated script replaces the driver's default
`-dT` script via an explicit `-T`.

Two different artifacts are embedded: the jobbin2 wrapper for SPURS execution,
and the original ELF with section headers for profiler image registration.
Registration and all six lane creations happen before enqueue. The jobheader
copy uses the generated sidecar's actual byte count into zeroed storage.

Before target use, inspect the SPU map, ELF sections/program headers/fixups,
wrapper output and disassembly. Check descriptor retention and relocation,
the `_start`/CRT layout, the 128-byte receipt layout, the writer/staging
alignment and disjoint extents, BSS initialization, and the 256-byte heap /
8192-byte stack metadata. Confirm every helper is PIC; the producer's memset
calls are redirected to the sample's independent PIC `sj_memset`. The sample
does not allocate profiler memory from the stack. All job LS allocations must
fit the actual allocated job region and the 221-KiB job-queue budget.

## Resource and completion contract

| Resource | Owner and rule |
| --- | --- |
| Lane | One fresh handle per invocation; no reattach or reuse |
| Ring/control | PPU profiler allocations, full 64-bit EAs in job payload |
| DMA tag | Caller supplies the job context's `dmaTag`; no guessed free tag |
| Writer | Per-job BSS object, 128-aligned, 128-byte guards before/after |
| Staging | Separate 16-aligned 320-byte buffer, 16-byte guards before/after |
| Receipt | One 128-byte line per job with separate 128-byte surrounding guards |
| Decrementer | Observe-only, attach flags zero; no write or OWN_DEC request |
| Events/reservation | Existing writer preflight; EBUSY is a refusal, never permission to change SPURS event masks |

The profiler also uses atomic reservation/status channels. Actual compatibility
with the scheduler's ownership of those channels is a target premise, not
established by using the job's DMA tag or by preserving its mask. The body does
not yield/suspend and keeps no writer alive across job returns.

The SPU waits for the result DMA before returning. The PPU reads receipts only
after the queue completion semaphore succeeds for all six submitted jobs. It
then closes, shuts down and joins the queue, and finalizes SPURS, checking every
return. Those teardown calls are cleanup facts; the release authority is the
separate completion-plus-receipt rule.

`common/protocol.h::sj_can_retire` requires queue completion, successful attach
and fini, the post-fini marker, and matching job/lane/capture-cookie/ring/control
identity. Failed fini, a missing receipt or an identity mismatch never calls
`lane_end`. All storage is static and remains borrowed until process exit on
failure. An early failure does not attempt profiler shutdown underneath a
possibly live producer. The sample does not reclaim unsubmitted lanes on an
incomplete setup. Queue/firmware/IO calls are blocking; an external run watchdog
must bound the experiment. No profiler timeout promises to bound them.

The receipt is explicitly reset at every job entry, including its completion
marker and attach/flush/fini statuses; cached image reuse does not rely on BSS
clearing to invalidate a previous invocation's successful receipt.

## Receipt checks and their limits

The capture is written under `/dev_hdd0/tmp`; its path is printed. A companion
`<capture>.spurs.json` is flushed, synced, and renamed from a temporary file.
The receipt records raw DEC observations, guards, masks, identities, call
results, and accepted-file versus post-completion producer counters.

```text
python -B host-tests/test_verify.py
python -B tools/verify.py <capture>.spurs.json --arm ON --build <64-hex> --session <16-hex>
```

Verifier exit 0 means the receipt checks passed and all three raw samples per
job decreased. It does **not** prove no hidden reset, correct timing, target
execution, or actual job overlap. Exit 2 means receipt checks passed but a
constant, increasing/reset-or-wrap, or partially stalled clock was observed:
timing is **INCONCLUSIVE**. Exit 1 means malformed/refused receipt evidence.
The OFF control uses `--arm OFF`.

The Python verifier checks receipt semantics only. Separately run the pinned
public reader's `check` and `spu-ticks --pairs --observations` on the exact
paired `.prof`; retain stdout, stderr and exit codes. Bind the capture,
receipt, SELF, raw ELF, jobbin2, maps, extension archive and reader hashes in
the external run packet. Inspect six unique lanes, the five named descriptor
sites, two balanced zone pairs per lane, seven ordinary events, finalization
and loss/refusal counters. Do not pass `--premise spu-nowrap` or an isolation
scale merely because this receipt passed.

`host-tests/lifetime.c` exercises the actual C retirement predicate, including
failed fini, missing receipt and reuse of a previously successful receipt after
the same reset helper used by the SPU entry. It is supplied for a later authorized native
host compile/run; Python tests do not execute that C code or the SPURS sample.
Linux CI can run `bash host-tests/run-host.sh /absolute/build/output` for both
the Python verifier and the native C predicate. This does not build a PS3 binary.
The target code still needs compile, artifact and runtime checks. Emulator
dispatch is not hardware evidence. Constant DEC on ordinary HEN must never be
relabelled zero-cost work; the sample makes no decrementer ownership change
to force it to advance. Tick values are not wall time or proven CPU time.

For EMP integration, register each actual job ELF before enqueue, assign a
fresh lane to each invocation, supply resource ownership explicitly and carry
the exact completion/receipt check into the job owner. This six-job sample
does not establish an unbounded queue integration; capture lane IDs are finite.
