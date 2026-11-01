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

## Tests

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror -fsanitize=address,undefined \
  samples/profiling/heterogeneous-workload/host-tests/test.c -o workload-test
./workload-test
python3 samples/profiling/heterogeneous-workload/host-tests/verify.py capture.csv --workers 6
```

Use the **observed** worker count, not an assumed six. The verifier requires all
four phases, matching per-worker/aggregate results and a successful END record.
Verify the `.prof` with the production reader separately. A successful emulator
run proves functionality, not hardware performance. Keep baseline/failed runs,
including a console hang, separate from accepted measurements.
