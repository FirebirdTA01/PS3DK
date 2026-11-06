# Candidate profiler file probe

This opt-in fixture builds the current profiler sources into a private test
archive. It links the selected SDK's installed `librt.a` through an explicit
library directory. It does **not** test an installed profiler archive, enable
automatic startup, implement compiler hooks or publish final capture names.

Configure each ABI and drain mode in a separate build directory:

```sh
cmake -S tests/sdk/ps3dk-prof-files -B "$PS3_BUILD_ROOT/prof-files-ilp32-app" \
  -G Ninja -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/ps3-ppu-toolchain.cmake" \
  -DPROFILER_ABI=ilp32 -DPROFILER_MODE=app
cmake --build "$PS3_BUILD_ROOT/prof-files-ilp32-app" --parallel 2
```

For LP64 select `cmake/ps3-ppu-toolchain-lp64.cmake` and `PROFILER_ABI=lp64`.
Select `PROFILER_MODE=thread` for the background drainer. The `.fake.self` is
the runnable artifact; the link map records the runtime provider.

The existing output directory defaults to `/dev_hdd0/tmp` and can be changed
with `PROFILER_TEST_DIRECTORY`. Each run creates a unique token and exclusively
creates retained `.probe`, `.prof`, `.spool` and `.arcs.pending` files. It never
deletes, overwrites or creates a final `.arcs` name. Budget about 70 KiB per run,
plus filesystem allocation overhead. A collision is an error, not permission
to truncate an existing file.

The primitive probe writes a 65,537-byte deterministic binary pattern through
a read/write exclusive descriptor, syncs, checks `EEXIST` on collision, rewinds
the same descriptor, compares every byte and checks EOF. A collision after
close is followed by sentinel readback. Each operation reports its result and
errno. The profiler test records two instances of one known arc per worker,
retires the worker, then exports and cleans up through the candidate adapter.
APP mode explicitly drains four successive incarnations through one slot;
THREAD mode uses one worker and shutdown joins its drainer. There is no test-only
private drain barrier.

Success requires `PROF_FILES_DONE ... fail=0` and target process exit 0. Copy the
exact `TIMING_PATH` and `ARCS_PATH` files printed by that run and check them:

```sh
ps3dk-prof check-pair capture.arcs.pending capture.prof
ps3dk-prof arcs-json capture.arcs.pending
ps3dk-prof report capture.prof
```

Require exit 0 from all commands. The arcs JSON must identify the expected ABI,
paired-capture mode 2, be complete and final, and contain four incarnations in APP mode or
one in THREAD mode. Each incarnation has one known edge from `0x4` to `0x8`,
count 2, and a retired final with attempted/recorded 2, refused/lost 0 and no
limit flags. These synthetic PCs measure serialization/counts, not compiler
instrumentation or symbolization. The `.probe` file must match byte `i*17 & 255`
for all 65,537 bytes. The timing HEADER's `consumer_mode` identifies APP (1) or
THREAD (2); this is distinct from the arc HEADER's pg-only/paired-capture `mode`.
After saving the `arcs-json` output, run the independent fixture checker:

```sh
python3 tests/sdk/ps3dk-prof-files/verify.py --abi ilp32 --mode app \
  --arcs-json arcs.json --timing capture.prof --probe retained.probe
```

The host checks supplement, rather than replace, target
exit and cleanup outcomes.

RPCS3 runs require the project's `rpcs3-claim.ps1` / `rpcs3-release.ps1` protocol,
a visible window and a per-run config that preserves `/dev_hdd0/tmp`. Keep the
shared config unchanged. RPCS3 `/dev_hdd0` is its VFS over the host filesystem:
these results do **not** measure real LV2 HDD/USB behavior, durability or speed.

Real-hardware acceptance remains a separate gate. Start with a matched no-input
idle control because HEN can fail independently. Record firmware/HEN state,
output device, ABI, mode, duration, TTY and retained file hashes. Measure
`O_RDWR|O_CREAT|O_EXCL`, collision preservation, sync and replay separately on
HDD and USB. The probe does not test hard links, rename or a publication
protocol; FAT32 output cannot rely on hard links. Installed-archive retention,
capacity/throughput measurements and a supported no-replace publication protocol
remain open before automatic startup integration.
