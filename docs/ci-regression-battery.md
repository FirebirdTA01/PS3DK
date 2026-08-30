# CI Regression Battery

## Goal

Catch runtime regressions that a sample build sweep cannot see. The first target
is the class found in the librt audit: type-clean wrapper drift that compiles and
links, then fails only when ordinary porting APIs are called under RPCS3.

## Scope

This is a small runtime battery, not a replacement for the full sample sweep.
Each probe must print one explicit `PASS` or `FAIL` line to TTY, then exit. Long
render-loop samples may be included only as named controls with fixed timeouts
and explicit non-terminal expectations.

The initial battery is seeded from `t_cd49e350`:

| Probe | Coverage |
|---|---|
| `librt-posix` | `gettimeofday`, `settimeofday` return path, `utime`, `umask`, `open`, `chdir`, `getcwd`, `opendir`, `readdir`, `telldir`, `rewinddir`, `seekdir`, `sbrk` ENOMEM, `socket`, `connect`, `send`, `close` |
| `tls` | PPU `__thread` local-exec TLS for scalar and aggregate objects; requires `TLS_OK` |
| `rsx-heap` | `rsxInit`, `rsxMemalign`, `rsxFree`, free-list coalescing, allocate/free stress |
| `rsx-wrap` | Command-buffer wrap past 1 MiB; seeded by `blitting` or a smaller dedicated wrap probe |
| `file-io` | `/dev_hdd0` create, read, write, seek, stat, close, unlink |
| `thread-sync` | PPU thread, mutex, condvar, join |
| `spu-roundtrip` | SPU image load/run/mailbox round-trip; seeded by `sputest` |

The probe sources should live under `tests/regression/`, with one subdirectory per
probe and a normal `CMakeLists.txt` using the SDK toolchain. The old diagnostic
`build/t_cd49e350_probe` is not a source of truth; its behavior should be moved
into `tests/regression/librt-posix/` and deleted from future reports.

## Harness Layout

Two scripts should share the same result schema:

| Script | Host | Responsibility |
|---|---|---|
| `scripts/build-regression.ps1` | Windows package extract | Configure and build every `tests/regression/*/CMakeLists.txt` against an extracted SDK |
| `scripts/run-regression-rpcs3.ps1` | Windows desktop RPCS3 | Boot selected `.fake.self` files, preserve logs, classify results |

The existing `scripts/boot-sweep.ps1` is the closest local pattern for the
runtime harness. The new regression harness should keep its useful behavior but make
the target list part of the repo instead of `C:\ps3boot\<label>\targets.txt`.

Proposed repo-owned target list:

```text
tests/regression/manifest.txt
```

Each row is:

```text
name,relative-self,timeout_seconds,expected_state,required_tty_regex,forbidden_tty_regex
```

Example:

```text
librt-posix,tests/regression/librt-posix/librt-posix.fake.self,25,RAN-CLEAN,librt-posix: PASS,ENOSYS
rsx-wrap,samples/PSL1GHT/graphics/blitting/blitting.fake.self,45,RENDER-LOOP,blitting started,ENOSYS
```

## Result Format

Every runtime result must keep three independent columns:

| Column | Meaning |
|---|---|
| `emulator` | Process outcome: `exited_early`, `ran_<N>s`, `exit_<code>`, `launch_failed` |
| `guest` | Probe outcome from TTY: `PASS`, `FAIL`, `NO_TTY`, or `UNCLASSIFIED` |
| `fatal` | Wide RPCS3 log classification from `RPCS3.log`: count and first matching line |
| `forbidden_tty` | Probe-specific forbidden TTY output such as `ENOSYS` |

This separation is mandatory. The blitting hang, the vp-loop guest fault, and
the POSIX socket ENOSYS failure all showed that emulator status, guest TTY, and
fatal log markers answer different questions.

The CSV header should be:

```text
name,self,emulator,guest,tty_lines,forbidden_tty_hits,first_forbidden_tty,fatal_hits,first_fatal,log_dir
```

The harness exits nonzero when any row has `guest=FAIL`, `guest=NO_TTY`,
`forbidden_tty_hits>0`, `fatal_hits>0`, `emulator=launch_failed`, or an
unexpected terminal state.

## Log Preservation

Before the first boot, copy the existing desktop RPCS3 `log/` directory to a
timestamped directory under the result root. Do not delete existing evidence.

For every sample:

1. Clear only the live `RPCS3.log` and `TTY.log` after the preservation copy.
2. Boot exactly one process with `--no-gui`.
3. Kill only the process started by the harness when the timeout is reached.
4. Copy `RPCS3.log`, `TTY.log`, and the harness metadata to the per-sample result
   directory.
5. Scan with the wide pattern used in the team rule:

```text
(^|[ \u00B7])F |Fatal|Access violation|frozen|Dead FIFO|recover_fifo|runtime_error|Emulation has been frozen
```

Do not put guest errno checks into the RPCS3-log fatal column. `ENOSYS` usually
appears only in the guest TTY, so it belongs in each row's
`forbidden_tty_regex` field. The default should be `ENOSYS`, with explicit
per-probe overrides for cases such as `settimeofday` on RPCS3, where the
emulator's HLE legitimately rejects the syscall and the probe treats the return
code as informational.

## Instance Rule

Runtime regression uses the desktop release RPCS3 only:

```text
C:\Users\FirebirdTA01\Desktop\Emulators\RPCS3\rpcs3.exe
```

The harness must fail preflight if any `rpcs3.exe` is already running. In human
runs, the operator must announce before booting and post "instance down" after
the process exits. CI runs satisfy the same rule by using a single self-hosted
runner with no parallel regression jobs.

The live-process check is necessary but not sufficient: a previous collision
happened between samples while no `rpcs3.exe` process was running. The runtime
harness must also take an exclusive lock file for the full run:

```text
C:\ps3boot\.rpcs3-owner
```

The lock contains the owner and timestamp. The harness refuses to start if it
exists, even when no emulator process is currently running. Human runs use the
same lock through `scripts/rpcs3-claim.ps1` and `scripts/rpcs3-release.ps1`, so
the room announcement and the machine-readable ownership state agree.

## CI Runner Question

There are two useful CI layers, and they should be separated:

1. **Push/PR CI, any hosted runner:** configure and build the regression probes in
   host-stub mode. This catches probe source and CMake bitrot on every change.
   It does not use the PS3 target compiler and does not prove SDK packaging or
   runtime behavior.
2. **Package build CI, any Windows runner with an SDK extract:** build
   `tests/regression/*` against a fresh package extract. This catches missing
   headers, missing libraries, and target-link bitrot. It does not prove runtime
   behavior.
3. **Runtime CI, Windows self-hosted runner:** run `scripts/run-regression-rpcs3.ps1`
   against the desktop release RPCS3 install. This is the only layer that can be
   the real runtime gate.

Until a Windows self-hosted runner is assigned, the first commit should still add
the push/PR host-stub job, the manual package-build job at
`.github/workflows/regression-build.yml`, and the local runtime harness. Do not
pretend GitHub-hosted CI has an RPCS3 oracle unless the runner actually has the
emulator and the package installed.

## First Implementation Slice

After this design is approved:

1. Move the `t_cd49e350` diagnostic behavior into
   `tests/regression/librt-posix/`.
2. Add `tests/regression/manifest.txt` with only `librt-posix` at first.
3. Add `scripts/build-regression.ps1` and `scripts/run-regression-rpcs3.ps1`.
4. Add a CI workflow or CI-ready script invocation that builds the probe on every
   push.
5. Run the probe locally on the desktop RPCS3 only after the current package
   re-cut/sweep is released.

Keep later probes (`rsx-heap`, `rsx-wrap`, `file-io`, `thread-sync`,
`spu-roundtrip`) as follow-up rows unless the director asks to broaden the first
slice.
