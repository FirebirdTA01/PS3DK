# Debugger: design study

Status: study only, nothing implemented. Written 2026-10-25 against `main` 8b0ffb20 for card
t_9313f2da ("ship GDB 14 properly in the SDK, with remote debugging on hardware and RPCS3,
on both hosts") and the worklist item "review PSL1GHT's recent debugger work".

Labels:
- **[measured]**: run or read by this study. Raw output is in the evidence directory
  `C:/wt/debugger-study-a` (sealed by its `INDEX.sha256`).
- **[public-cited]**: a public source cited by repository, revision and file:line.
- **[unverified]**: plausible, not yet tied to a measurement or an exact source.
- **[estimate]**: judgement.

## 1. What ships today

**The native Linux build includes GDB; the Windows SDK does not.** Published tarball
contents are unverified.
- `scripts/build-ppu-toolchain.sh:596-605` returns early when `HOST_TRIPLE` is set, so the
  Windows cross build never builds GDB ("mingw-w64 GMP/MPFR not yet packaged; deferred").
  The installed v0.26.0 Windows SDK has no `gdb` executable at all [measured: only
  `libstdc++.a-gdb.py` pretty-printer hooks exist under `ppu/` and `spu/`].
- The native Linux build runs every step (`.github/workflows/release.yml:560` runs
  `./scripts/build-ppu-toolchain.sh` without `--only`), so `ps3dev-ppu.tar.xz` should carry
  `ppu/bin/powerpc64-ps3-elf-gdb` [unverified for the published tarball; measured in our WSL
  stage `ehr2-base-s`, which the same script built].
- Patches: one, `patches/ppu/gdb-14.x/0001-py-gdb-readline-python-3.13-compat.patch`, a host
  build fix for Python 3.13. There is no PS3-specific GDB code.

**The Linux GDB itself** [measured, `linux/gdb-version.txt`, `gdb-config.txt`]:
- GNU gdb 14.2, `--target=powerpc64-ps3-elf`, built with Python (`python print(gdb.VERSION)`
  works), expat (so XML target descriptions work), curses/TUI; no lzma, debuginfod or
  babeltrace. No `gdbserver` (`--disable-gdbserver`), which is correct: there is no LV2 host
  for it.
- The configure-time data directory points at the stage it was built in
  (`ehr2-base-t/.../share/gdb`), marked relocatable. Pretty-printer auto-load from a moved
  install is therefore worth a test [unverified].

**DWARF from GCC 16 reads correctly in both ABIs** [measured, `linux/gdb2-*.txt`, sample
`linux/t.c` built with `-g -O0`]:

| Check | ILP32 (default) | LP64 (`-mlp64`) |
|---|---|---|
| `ptype struct pt` | all four members | all four members |
| `sizeof(struct pt)` | 16 | 32 |
| `sizeof(char *)` | 4 | 8 |
| `print g` | correct values and string | correct |
| `break add` / `break t.c:4` | resolves to the post-prologue address | same |

**One real defect: function symbols resolve to descriptors.** The PS3 ELFv1 variant uses
8-byte function descriptors (32-bit entry, 32-bit TOC; `.opd` contents in
`linux/opd-ilp32.txt` and `opd-lp64.txt` show `00010230 00049400`), in both ABIs.
GDB's ppc64 descriptor code reads
them as 64-bit entries [measured: the minimal symbol for `add` is listed at
`0x00010b6400049400`, i.e. entry and TOC glued together, and `info line add` falls back to
the `.opd` address `0x40250`]. In these probes, with DWARF, `break add` and `break t.c:4`
both resolved to the correct post-prologue address in both ABIs, and `print &add` gave the
entry `0x10b64` (ILP32) / `0x10854` (LP64) [measured, `linux/gdb3-*.txt`]. That does not
clear the defect: indirect calls through descriptors, calls into code without debug info
(system libraries, stripped code), backtraces through such frames, and anything else that
uses minimal symbols are not covered by those probes and should be expected to misresolve. Fixing this is a small, contained GDB patch in
`ppc64-tdep.c`/`ppc-sysv-tdep.c` descriptor handling for the `powerpc64-ps3-elf` OSABI
[estimate: S-M].

**ELF identification.** Our executables carry `OS/ABI 66` (CellOS LV2) [measured,
`readelf -h`]. GDB has no OSABI sniffer for it and reports `osabi none` [measured]. A
`cell-lv2` OSABI registration is the natural hook for the descriptor fix and for a later
target description.

**The debug ELF is not the shipped `.elf`.** `ps3_add_self()` writes a *stripped* `.elf`
next to the SELF; the unstripped executable stays in the build directory
(`cmake/ps3-self.cmake:14-15`). Loading the stripped one gives "No symbol table is loaded"
[measured, `rpcs3-b-*`]. Docs and any launch tooling must point GDB at the build-tree ELF.

**SPU: no GDB.** GDB removed Cell/B.E. support, standalone SPU and combined PPU/SPU, in
GDB 9: `gdb/NEWS:1901-1905` at tag `gdb-14.2-release` (f0886172) [public-cited]. GDB 14.2
lists no SPU architecture [measured, `set architecture`]. SPU debugging would need either
an old GDB (8.3.1, see §3.3) or our own SPU target work; see §5.

## 2. RPCS3 as a target

**Built in and on by default.** The installed RPCS3 v0.0.42-19958 (54014a7d) contains the
GDB server (`rpcs3/Emu/GDB.cpp`, 1014 lines at that commit) [public-cited], configured by
`Misc: GDB Server: 127.0.0.1:2345` (`rpcs3/Emu/system_config.h:379`) [public-cited]. It
listens as soon as a game boots: `GDB: Started listening on '127.0.0.1:2345'` [measured].

**Packets supported** (`GDB.cpp:966-984`) [public-cited]: `!`, `?`, `qSupported`,
`qfThreadInfo`, `qC`, `p`/`P`, `m`/`M`, `g`/`G`, `H`, `qAttached`, `k`, `vCont?`/`vCont`
(`c`, `s`), and `z0`/`Z0` software breakpoints only. **No `D` (detach)**, no hardware
breakpoints or watchpoints (`Z1`-`Z4` get an empty reply, `GDB.cpp:882`), no conditions
(`E01`), and PPU threads only: the SPU thread enumeration is commented out
(`GDB.cpp:582`).

**Breakpoints do nothing under the LLVM PPU recompiler.** `ppu_breakpoint()` returns false
when `g_cfg.core.ppu_decoder == ppu_decoder_type::llvm` (`rpcs3/Emu/Cell/PPUThread.cpp:1232`)
[public-cited], but `cmd_set_breakpoint` replies `OK` anyway (`GDB.cpp:879-880`). Measured
exactly that: with the default LLVM decoder, `Z0,10b98` got `OK` and the program ran on past
it 60 times (`rpcs3-c-*`); with `PPU Decoder: Interpreter (static)` passed through
`--config` the breakpoint fired (`rpcs3-d-*`).

**What works with the PPU interpreter** [measured, `rpcs3-d-ilp32/gdb.out`,
`rpcs3-d-lp64/gdb.out`, our Linux GDB 14.2 through a TCP relay]:

| Operation | ILP32 | LP64 |
|---|---|---|
| attach (`target remote`), initial stop with source line | yes | yes |
| `break add` (DWARF), `continue`, stop at the breakpoint | yes | yes |
| `info registers` (pc, r1-r4, lr) | correct | correct |
| `bt` with arguments | correct to `main`, then `._start` | same |
| `x/4wx &g`, `print g` | correct | correct |
| `stepi` | **did not advance**: `vCont;s` got an immediate `S05`, PC unchanged (one test, cause unverified) | same |
| `detach` | **not supported** ("Remote doesn't know how to detach") | same |

Practical notes:
- The address is configurable: `GDB.cpp:119-159` parses an IPv4 `address:port` from the
  setting [public-cited]. The default, and the study's configuration, binds 127.0.0.1:2345.
  With that binding, WSL2 in NAT mode cannot reach it directly; the study used a
  small Windows-side TCP relay (`tools/relay.py`). A Windows-hosted GDB, or WSL mirrored
  networking, avoids the relay.
- Debug runs need the PPU interpreter, which is far slower. A per-run `--config` copy keeps
  the shared RPCS3 configuration untouched.

## 3. Hardware (HEN, no DEX/DECR)

### 3.1 What a retail console offers

There is no hardware debug channel on a retail console. The two candidate in-process
mechanisms are:

- **lv2dbg.** We already ship the headers and stubs: `sdk/include/sys/dbg.h` (280 lines at 8b0ffb20) over
  `liblv2dbg_stub.a`, loaded as `CELL_SYSMODULE_LV2DBG` [measured]. It declares:
  - PPU exception-handler registration (`sys_dbg_register_ppu_exception_handler`, `:219`);
  - thread enumeration and context reads for PPU and SPU (`:210-214`, `:239-255`);
  - sync-object inspection;
  - `sys_dbg_set_address_to_dabr` (`:266`), a data breakpoint.

  **Whether the module loads, and these calls succeed, on a retail kernel with HEN is
  unverified.** This is the single most important unknown; a one-SELF hardware probe answers
  it (§5, phase 1).
- **Trap-based breakpoints in our own process.** Writing a `tw`/`trap` over code needs
  writable text, which the loader maps read-only on retail [unverified], plus a handler to
  catch the trap. Without the exception handler above, a trap kills the process.

### 3.2 Recommended hardware shape

An **in-app GDB stub** linked into the debugged program (`-lps3dkdbg` or a CMake option):
- It speaks the GDB remote protocol over the ps3dk-run protocol-2 transport and pairing, or
  over a plain TCP port for local networks. Protocol 2 is installed on the console as the
  loader. The 2026-10-25 session (`C:/wt/hw-lsput-20261025-c`) launched a control child that
  exited with `EXITED 7` and `loader_ready` true, then a probe whose transport outcome was
  `NO_HOOK` and whose result came from a file log. That shows launch and pairing, not a
  debug channel: multiplexing a debug stream over protocol 2 is unimplemented.
- Breakpoints: software traps if text can be made writable, otherwise DABR for one data
  watchpoint and "break on entry/exception" only. All of these assume lv2dbg works (§3.1).
- Exceptions: **if** lv2dbg is available, its PPU exception handler converts faults into GDB
  stop replies with a full register set, which alone is a large improvement over today's
  silent crashes. If lv2dbg is not available on retail/HEN, that handler is not available
  either, and the stub cannot promise exception catching; what remains would have to be
  established separately.
- Threads: `sys_dbg_get_ppu_thread_ids` + context reads give `info threads` and `bt` per
  thread.
- SPU: thread and group enumeration and context reads are declared; stopping and stepping
  SPUs is out of scope until a GDB with SPU support exists (§1).

### 3.3 PSL1GHT / ps3dev review

- **PSL1GHT:** the vendored checkout is `eca3f990a6` (2024-10-20, `ps3dev/PSL1GHT`).
  Searched on 2026-10-25, upstream head `f649a08fd5`:
  - the first 100 commit messages since 2020-01-01, matched for gdb, debug, stub, exception
    and trap, gave only `99dd0b91ef` (chdir/getcwd) and `40879a2411` (rsx methods);
  - the commits since 2024-06-01 had no debugger subject either;
  - the vendored tree's file contents, grepped for gdbstub, gdb_stub, gdbserver and
    `qSupported` (case-insensitive `git grep`, rc 1), had no matches
    (`psl1ght-grep.txt`, empty).

  These searches found no debugger work [public-cited, GitHub commit API]. That is a
  bounded search, not proof that none exists.
- **ps3toolchain (`ps3dev/ps3toolchain`)** restored its GDB build scripts on 2026-10-11
  (merge `de85d7d5d1`, "restore_gdb") [public-cited]:
  - `scripts/003-gdb-PPU.sh` and `scripts/007-gdb-SPU.sh` build **GDB 8.3.1** for
    `powerpc64-ps3-elf` and `spu`;
  - their `patches/gdb-8.3.1-PS3.patch` touches only host-build files: Makefile, Python glue,
    threading and enum-flags [measured file list]. There is no remote stub or LV2 support.

  Take-away: GDB 8.3.1 is the last release with an SPU target, which is why they pin it.
  Shipping a second, old SPU-only GDB is possible but buys only static SPU ELF inspection
  until there is a target to talk to [estimate].

## 4. Host side

- **Linux:** the native build includes GDB (published `ps3dev-ppu.tar.xz` contents
  unverified). `powerpc64-ps3-elf-gdb build/app` then
  `target remote <host>:2345` (RPCS3) or the stub's port (hardware).
- **Windows:** no GDB in the zip. Options:
  - cross-build GDB for mingw-w64 with GMP/MPFR (the deferred item at
    `build-ppu-toolchain.sh:598`), without Python first;
  - or document WSL-hosted GDB plus the relay/mirrored networking.

  The first is the real fix [estimate: M].
- **VS Code:** a `cppdbg` launch configuration is enough:
  - `miDebuggerPath` set to the PPU GDB;
  - `program` set to the build-tree (unstripped) ELF;
  - `miDebuggerServerAddress` set to `localhost:2345`;
  - `setupCommands` for the source path map.

  A sample `launch.json` should ship in the SDK's samples [estimate: S].
- **ps3dk-run:** add `ps3dk-run --debug`. It launches the app on the console with the stub
  enabled, waits for the stub's ready message, and prints, or starts, the matching
  `gdb -ex 'target remote ...'`.

## 5. Recommended plan

| Phase | Work | Test | Effort |
|---|---|---|---|
| 0 | Docs: debugging with RPCS3 today (PPU interpreter via `--config`, unstripped ELF, no detach, LLVM ignores breakpoints) | doc review | S |
| 1 | Hardware probe SELF: load `CELL_SYSMODULE_LV2DBG`, call thread-id, context-read, exception-handler and DABR functions, log results to `/dev_hdd0/tmp` | one ps3dk-run protocol-2 launch, control first | S |
| 2 | GDB patch: `cell-lv2` OSABI sniffer (OS/ABI 66) and 8-byte descriptor handling, both ABIs | host test on stripped/unstripped ELFs: symbol addresses, `break` by name without DWARF | S-M |
| 3 | Windows GDB: mingw-w64 GMP/MPFR prerequisites, GDB in the Windows zip, release artifact gate | package test + attach to RPCS3 from Windows | M |
| 4 | In-app stub (depends on phase 1): RSP core, registers, memory, exceptions, threads, over TCP and protocol 2 | host unit tests of the RSP parser, RPCS3 run, hardware run | L |
| 5 | `ps3dk-run --debug`, VS Code sample config, user guide | hardware launch-and-attach | S-M |
| later | SPU: inspection via lv2dbg context reads in the stub; a GDB with an SPU target is a separate, large project | — | L+ |

Risks:
- lv2dbg may be unavailable on retail/HEN. The exception handler is itself an lv2dbg call,
  so in that case even exception catching is not available, and the in-app stub design
  would need a different mechanism or would shrink to memory/register inspection the app
  does for itself. Phase 1 decides.
- RPCS3's stub is limited (no detach, `stepi` did not advance in the one test, LLVM decoder
  ignores breakpoints). Upstream fixes to RPCS3 are possible but outside our control.
- The interpreter-only debug runs are slow; long programs need targeted breakpoints.

Decisions for the director:
1. Order: start with phases 0-2 (cheap, independent), or put the Windows GDB (phase 3) first?
2. Is an in-app stub acceptable as the hardware debugging model (the app links a debug
   library), or should a separate loader-side approach be researched first?
3. SPU: accept "inspection only" for now, or fund a GDB SPU target?
4. RPCS3: should we propose upstream fixes (honest `Z0` reply under LLVM, `D` support,
   stepping), or just document the limits?
