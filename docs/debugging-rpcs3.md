# Debugging with RPCS3 today

This guide covers source-level debugging of a PPU program running in RPCS3,
using the GDB server that RPCS3 has built in. It describes what works now,
without new tooling. The plan for hardware debugging and a Windows GDB is in
[design/debugger-design.md](design/debugger-design.md).

Every statement below comes from the debugger design study, run on
2026-10-25 with RPCS3 v0.0.42-19958 (54014a7d) and GDB 14.2 from our Linux
toolchain build. Its raw output is kept with the study's evidence; the
study names it per claim. Anything that was not measured says so.

## What you need

- **A GDB for `powerpc64-ps3-elf`.** The native Linux toolchain build
  produces `ppu/bin/powerpc64-ps3-elf-gdb` (GDB 14.2, with Python and XML
  target descriptions). The Windows SDK zip has no GDB yet; that is phase 3
  of the plan. Whether the published Linux tarball carries GDB has not been
  checked.
- **The unstripped ELF.** `ps3_add_self()` writes a stripped `.elf` next to
  the SELF, and GDB finds no symbols in it ("No symbol table is loaded").
  Point GDB at the executable CMake builds in the build tree: the target
  name with no extension. Build with `-g`, and preferably `-O0`, for
  readable stepping.
- **RPCS3 with its GDB server on.** It is on by default and listens on
  `127.0.0.1:2345` as soon as a game boots (the log shows
  `GDB: Started listening on '127.0.0.1:2345'`). The address is the
  `Misc: GDB Server` setting.

## Use the PPU interpreter

Breakpoints do nothing under RPCS3's default LLVM PPU recompiler. RPCS3
answers `OK` to the breakpoint request, but the program runs straight past
it. In the study's test it passed the breakpoint 60 times.

Run debug sessions with the interpreter instead. To leave your normal
configuration alone, copy RPCS3's `config.yml`, set

```yaml
Core:
  PPU Decoder: Interpreter (static)
```

and boot with that copy:

```
rpcs3 --no-gui --config <copy>.yml <path-to>.self
```

The interpreter is much slower than LLVM. For long programs, break near the
code you care about.

## Connect

With the program booted:

```
powerpc64-ps3-elf-gdb build/<target>
(gdb) target remote 127.0.0.1:2345
(gdb) break add
(gdb) continue
```

RPCS3 binds the loopback address by default. A GDB inside WSL2 in NAT mode
cannot reach it directly: the study used a small TCP relay on the Windows
side. WSL mirrored networking, or a GDB that runs on Windows, should avoid
the relay; neither was tested.

## What works

Measured with the PPU interpreter, in both data models (ILP32, the default,
and `-mlp64`):

| Operation | Result |
|---|---|
| `target remote`, initial stop with the source line | works |
| `break <function>` / `break file.c:line` (DWARF), `continue` | stops at the breakpoint |
| `info registers` (pc, r1-r4, lr) | correct |
| `bt` with arguments | correct up to `main`, then `._start` |
| `print`, `x/4wx` on globals | correct |
| `ptype`, `sizeof` of structs and pointers | correct for each data model |

## Known limits

- **No detach.** RPCS3 does not implement detach; GDB reports "Remote
  doesn't know how to detach". End a session with `kill`, or close RPCS3.
- **`stepi` did not advance** in the one test made: the step request got an
  immediate stop with the PC unchanged. The cause has not been
  investigated. Use breakpoints and `continue` in the meantime.
- **Software breakpoints only.** Hardware breakpoints and watchpoints are
  refused, and breakpoint conditions are not supported by RPCS3's server
  (from RPCS3's source; not exercised).
- **PPU threads only.** RPCS3 does not list SPU threads to GDB, and GDB 14
  has no SPU support.
- **Function symbols without debug info resolve wrongly.** PS3 executables
  use 8-byte function descriptors (a 32-bit entry and a 32-bit TOC) in both
  data models. GDB reads the descriptor's first 8 bytes as a 64-bit entry
  address, so a minimal symbol shows the entry and TOC glued together. Breakpoints and backtraces through code built with `-g` were
  correct in the study. Calls through function pointers, and code without
  debug info (system libraries, stripped objects), are expected to resolve
  wrongly but were not tested. A GDB fix is phase 2 of the plan.
