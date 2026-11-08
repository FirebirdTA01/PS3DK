# HEN heterogeneous workload profiler sample

A standalone, instrumented homebrew example for a non-DEX PS3. It repeats:

| Phase | Compute placement | RSX scene |
|---|---|---|
| 0 | PPU serial | Light gradient, one layer |
| 1 | Persistent SPE workers | Same light scene |
| 2 | Persistent SPE workers | Procedural fragment shader, eight overlapping layers |
| 3 | PPU serial | Same heavy scene |

Each frame computes the same 16,384-item, 128-round integer workload. Results
must have digest `e7b15af8` and the correct item count in either compute mode.
The SPE pool partitions indices across up to six workers and displays how many
it actually obtained. No available workers is an explicit failure. This is a
synthetic workload; it does not predict the speedup of a particular game.

The default run has a five-second idle control, eight-second phases and a
120-second session. START or XMB Quit exits. Set `SAMPLE_SESSION_SECONDS=0`
explicitly for continuous looping. Worker and presentation polls have two-second
deadlines. A syscall blocked inside the OS cannot be interrupted by those polls.

## Build

This branch currently requires a **candidate SDK** containing `ps3dk/prof.h`
and both ABI versions of `libps3dkprof.a`; it is not a released SDK feature.
Use the native Windows PS3DK or the Linux toolchain, with matching runtime and
SPU support. It does not require a DEX console or proprietary SDK code.

```sh
cmake -S samples/profiling/heterogeneous-workload -B "$PS3_BUILD_ROOT/heterogeneous" \
  -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/ps3-ppu-toolchain.cmake" \
  -DSAMPLE_ABI=ilp32 -DSAMPLE_BUILD_ID=your-exact-source-id
cmake --build "$PS3_BUILD_ROOT/heterogeneous" --parallel 2
```

For LP64, select `ps3-ppu-toolchain-lp64.cmake` and `SAMPLE_ABI=lp64` together.
`SAMPLE_PROFILING=OFF` builds the same workload without the profiler archive;
the header's disabled macros evaluate no zone arguments. Runtime, CRT and SPU
code do not receive global `-pg` or `-finstrument-functions` flags.
`SAMPLE_SPU_SDK` normally equals `PS3DK`; it can explicitly select a complete
SPU SDK when testing a partial private PPU candidate install.

Outputs include `.self` and `.fake.self`. Use the project's normal signed
homebrew launch path on HEN and its RPCS3 launch/lease procedures for emulation.
Short functional runs can set phase/session/idle seconds to `2/12/1`.

## Capture and interpretation

The sample owns one manual APP-mode PPU profiler lifecycle. It drains after
each frame and shuts down after its SPE pool stops. Named scopes cover serial
PPU compute, SPE dispatch/wait, render submission and flip wait. Completed
captures are written under `/dev_hdd0/tmp/heterogeneous-*.prof` and can be
retrieved through FTP. Validate before interpreting:

```sh
ps3dk-prof check capture.prof
ps3dk-prof report capture.prof
```

An independently created `heterogeneous-*.csv` contains per-frame compute and
frame elapsed microseconds, per-SPE count/digest/decrementer readings, and raw
GPU scene timestamps. It is sample telemetry, not an extension to the frozen
PPU capture format. Logging occurs outside the measured compute/scene intervals;
file output and profiler draining still contribute to total application overhead.
`frame_us` spans callback polling, compute and rendering/presentation, ending
before telemetry export/drain. It is not a refresh-rate or dropped-frame metric.

RSX reports bracket scene draws and exclude the text overlay. The sample waits
for its own flip callback, cleared flip status and sequence-specific backend
completion label before reading/reusing report slots. The display shows the
prior scene's raw delta, or `unavailable`; no tick-to-time calibration is claimed.
SPE decrementer values are local per-job observations, not a synchronized global
timeline. PPU wait is not pure SPE compute, and GPU flip wait is not GPU busy time.
The full calibrated PPU/SPU/RSX backend is still being developed.

CPU and SPE execution share unsigned arithmetic. A volatile local seed prevents
the synthetic kernel from being cached across identical repeated jobs. There is
one in-flight batch, monotonically increasing nonzero sequence numbers, separate
payload/commit cache lines, and a DMA completion before publishing the commit.
There is no per-job allocation or worker creation.

## Live view (SAMPLE_LIVE)

`-DSAMPLE_LIVE=ON` (it needs `SAMPLE_PROFILING=ON`; configure refuses it
otherwise) lets the PC viewer watch the capture while the sample runs. The
console serves the `.prof` file the profiler is writing, over TCP port 31750,
to one viewer at a time. The screen shows the address to connect to (or why the
live view is off); in the viewer, choose Connect and enter it.

**Status: not yet validated on the console.** What has been measured on HEN
(internal HDD) is narrower: an app thread can read the profiler's open capture
file while it grows. The live view itself (TCP serving, byte equality with the
console file, overhead) is still pending its target gates.

The viewer keeps a complete copy only when the stream ends properly: the
console sends END after the profiler's shutdown, the connection closes cleanly,
the reader accepts the capture, and the viewer's local checks pass before it
publishes the file. A slow or disconnected viewer keeps an incomplete copy,
labelled as such, when its local saving succeeds. The agent only reads the file and never changes what is
written, but it still uses CPU, memory and I/O: a live failure does not by
itself fail the file capture, and neither a failed start nor the agent's
read-only design guarantees a valid capture or zero perturbation.

At exit the sample waits nominally up to 2 s for a connected viewer to receive
END, 1 s for the agent thread, and once more 2 s if it is still stopping. These
are not a hard exit bound: a console file call already in progress and the
final ordinary join are not covered by them.

Every network and live result (network init, live start, each stop attempt,
each network shutdown attempt, and whether the network was left initialized)
is written to `/dev_hdd0/tmp/heterogeneous-<id>.live.txt` next to the CSV, so
it can be read after the console returns to the XMB. It is separate from the
workload result and the profiler's verdict.

The calls an app adds, in order (`source/live.c` is the sample's complete
version, with every result checked and recorded):

1. After `ps3dk_prof_init`: `netInitialize`. If it fails, skip the live view.
2. `ps3dk_prof_live_start` (a zeroed config takes the defaults). If it fails,
   keep running and call `netDeinitialize` at once: no agent thread exists.
3. At the end: `ps3dk_prof_shutdown` first, so a connected viewer can get END.
4. `ps3dk_prof_live_stop`; on `ETIMEDOUT` or `ECLOCK` call it again. Only after
   `OK` call `netDeinitialize`; after `ETHREAD`, or a stop that never returns
   `OK`, leave the network initialized.

Link `ps3dkprof`, `net` and `netctl_stub` (the sample asks netctl for the
console's address). Only captures on the internal HDD (`/dev_hdd0/`) are
served: the device on which concurrent capture-file visibility was measured;
target live-link validation remains pending. `SAMPLE_PROF_SDK` selects a candidate profiler overlay (its
`ppu/include/ps3dk/prof.h` and `ppu/lib[/lp64]/libps3dkprof.a`) ahead of the
SDK's.

## SPU lanes and RSX domains (SAMPLE_PROF_EXT)

`-DSAMPLE_PROF_EXT=ON` (ILP32 or LP64, needs `SAMPLE_PROFILING`) records each SPU worker's jobs as `spu.work` zones
on its own SPU lane and the scene's command blocks as `rsx.setup` / `rsx.draw` zones on an RSX domain, so the
viewer's Timeline shows PPU, SPU and RSX rows.  By default it links the SDK's installed extension archive of the
selected data model, `libps3dkprof_ext.a` in `$PS3DK/ppu/lib` (ILP32) or `$PS3DK/ppu/lib/lp64` (LP64), and with
`SAMPLE_LIVE` the installed `libps3dkprof_ext_live.a` beside it, using the installed `ps3dk/prof_spu.h` and
`ps3dk/prof_gpu.h` (`make -C sdk/libps3dkprof install` puts all of these in place for both data models;
`SAMPLE_PROF_SDK` selects an overlay root of the same layout):

```sh
cmake -S samples/profiling/heterogeneous-workload -B "$PS3_BUILD_ROOT/heterogeneous-ext" \
  -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/ps3-ppu-toolchain.cmake" \
  -DSAMPLE_ABI=ilp32 -DSAMPLE_PROF_EXT=ON -DSAMPLE_LIVE=ON -DSAMPLE_BUILD_ID=your-exact-source-id
cmake --build "$PS3_BUILD_ROOT/heterogeneous-ext" --parallel 2
```

For LP64 select `ps3-ppu-toolchain-lp64.cmake` with `SAMPLE_ABI=lp64`.  `SAMPLE_EXT_ARCHIVE` (with `SAMPLE_LIVE`,
also `SAMPLE_EXT_LIVE_ARCHIVE`) explicitly selects a build-tree archive instead (`make -C sdk/libps3dkprof ext-ilp32`
or `ext-lp64`, `ext-live-ilp32` or `ext-live-lp64`); the extension headers then come from `SAMPLE_PROF_SRC`, which
must be set.  The SPU worker links the installed SPU lane library `$PS3DK/spu/lib/libps3dkprof_spu.a` (headers in
`$PS3DK/spu/include/ps3dk`); setting `SAMPLE_PROF_SRC` to `sdk/libps3dkprof/src` is a developer override that compiles
the lane writer from those sources instead.  Configure refuses a `SAMPLE_ABI` that is not the compiler's data model and an archive
member whose data-model tag is not `SAMPLE_ABI`'s.  The SPU worker takes its work-slot and control-block addresses as
32-bit values in both data models: the PPU side refuses (logged as `POOL pack-refused`) any address above 4 GiB
instead of narrowing it, and a refusal turns SPU profiling off and clears `w32_eligible`.  The SPU decrementer is reset at least every 20 s per
worker (idle workers get reset-only commands), so every SPU epoch spans under 22 s, well inside the 32-bit wrap
(53.8 s): the run prints `w32_eligible=1` when every check held, and only then is the viewer's W32 premise
justified for that capture.  The GPU domain uses reports 32..63 and label 241; the application fence uses label
242.  Status: ILP32 has run on a HEN console; LP64 is host-tested and target-compiled, and its console run is
pending.

## Tests

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  samples/profiling/heterogeneous-workload/host-tests/test.c -o workload-test
./workload-test
python3 samples/profiling/heterogeneous-workload/host-tests/verify.py capture.csv --workers 6
```

The SPU-lane/RSX-domain lifecycle (`SAMPLE_PROF_EXT`) has its own host test against a mocked profiler:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined   samples/profiling/heterogeneous-workload/host-tests/test_prof_ext.c   samples/profiling/heterogeneous-workload/source/prof_ext.c   samples/profiling/heterogeneous-workload/source/pool_proto.c   samples/profiling/heterogeneous-workload/source/worker_proto.c -pthread -o prof-ext-test
./prof-ext-test
```

Use the **observed** worker count, not an assumed six. The verifier requires all
four phases, matching per-worker/aggregate results and a successful END record.
Verify the `.prof` with the production reader separately. A successful emulator
run proves functionality, not hardware performance. Keep baseline/failed runs,
including a console hang, separate from accepted measurements.
