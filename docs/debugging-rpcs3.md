# Debugging PPU programs in RPCS3

RPCS3 contains a GDB remote server. The SDK's PPU GDB (`powerpc64-ps3-elf-gdb`,
GDB 14.2) attaches to it, so a program can be stopped and inspected on the desktop
without a console. Stepping is limited by the emulator, as described below.

## One step

Add one line after `ps3_add_self()`:

```cmake
ps3_add_self(my_app)
ps3_add_debug_rpcs3(my_app)                 # stops at main
# ps3_add_debug_rpcs3(my_app BREAK update)  # or at another function
```

Point the launcher at RPCS3 once (Windows: `setx PS3DK_RPCS3 C:\path\to\rpcs3.exe`,
Linux: `export PS3DK_RPCS3=/path/to/rpcs3`), then:

```sh
cmake --build build --target debug-my_app
```

This builds the program, boots its `.fake.self` in RPCS3 and starts GDB on the
generated `build/my_app.gdbinit`, which:

- loads the **unstripped** executable from the build directory (the `.elf`
  next to the SELF is stripped and has no symbols),
- connects to RPCS3's GDB server (`127.0.0.1:2345` by default; change it with
  `ps3_add_debug_rpcs3(my_app GDB_SERVER address:port)`),
- releases the pre-main wait by setting `__ps3dk_debug_go`,
- sets the breakpoint.

From there: `continue`, `bt`, `info locals`, `print`, `x/4wx &var`.
Before continuing from a breakpoint, disable or delete that breakpoint; see
the workaround below.
`samples/debug/hello-debug-rpcs3` is a small program to try this on.

## What the launcher changes, and what it does not

RPCS3 is started with a per-run copy of its own configuration in which exactly
two settings differ:

| Setting | Debug run | Why |
|---|---|---|
| `PPU Decoder` | `Interpreter (static)` | RPCS3 ignores breakpoints under the LLVM recompiler (it acknowledges them and runs on). |
| `GDB Server` | the requested address | where GDB connects |

The copy lives in the temporary directory and is deleted afterwards. Your RPCS3
configuration is never written. The interpreter is much slower than the
recompiler, so debug runs are slower than normal runs.

RPCS3 starts a command-line-booted program immediately, even with its auto-start
setting disabled. The launcher uses `--no-gui` and creates `ps3dk-debug-wait`
beside the SELF before boot. The CMake helper adds a constructor that detects
this marker through `/app_home` and waits for GDB, for up to 120 seconds. The
generated command file releases that wait. Without the marker, the constructor
returns immediately, including during normal runs on hardware.

A hard kill (such as closing the launcher terminal or `kill -9`) can leave the
marker behind. A later plain run of that SELF from the same build directory
then waits up to 120 seconds before main too. If no debug session is running,
remove `ps3dk-debug-wait` from that directory before running normally.

The launcher removes its marker when the session ends or startup fails. It
refuses to overwrite an existing marker, so two sessions cannot own the same
SELF directory. After a forced termination that prevented cleanup, remove a
stale marker only after confirming its debug session has ended.

## Limits of RPCS3's GDB server

Measured with RPCS3 v0.0.42-19958 (54014a7d), interpreter mode, and GDB 14.2:

- **No detach.** When the launcher starts GDB, it closes its RPCS3 process after
  GDB exits. For an external GDB (`-NoGdb` / `--no-gdb`), press Ctrl-C in the
  launcher terminal to end the session; there is no RPCS3 window to close.
- **Software breakpoints only.** No hardware breakpoints or watchpoints, and no
  breakpoint conditions evaluated by the server (GDB-side conditions work).
- **PPU threads only.** SPU threads are not listed or debuggable.
- **Stepping and resuming at an enabled breakpoint are unreliable.** `next`,
  `finish`, and `continue` repeatedly stopped at the same PC in both ABIs.
  The earlier `stepi` measurement also failed to advance. Disabling the
  breakpoint before `next` did not make stepping usable in the follow-up test.
  These commands are not validated as working.

The measured workaround is to disable or delete the breakpoint at the current
PC, set a temporary breakpoint at a later source location, and use `continue`:

```gdb
disable 1
tbreak main.c:33
continue
info locals
```

For the sample, line 33 is the return from `accumulate`; `sum` is now 3 on the
first call. Re-enable breakpoint 1 after leaving its address if you want to
inspect a later call. Temporary breakpoints delete themselves when hit.
This flow was checked on both ABIs through totals 3, 73, and 1173. It does not
substitute for a working single-step implementation. RPCS3's
[`cmd_vcont` implementation](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/GDB.cpp)
handles the `s` request with `dbg_step`; the recorded failures remain an emulator
compatibility limit, not a claim that its precise cause has been fixed.

## GDB on another host

If GDB runs elsewhere (for example a Linux GDB inside WSL), start RPCS3 only:

```powershell
scripts\ps3dk-debug-rpcs3.ps1 -Self build\my_app.fake.self -GdbInit build\my_app.gdbinit -NoGdb
```

Leave that terminal open while using the external GDB. Press Ctrl-C there to
stop the owned RPCS3 process and remove the marker and temporary configuration.
If RPCS3 exits on its own, the launcher returns its exit status.

and attach with `powerpc64-ps3-elf-gdb -x build/my_app.gdbinit`. RPCS3 listens on
`127.0.0.1` by default, which a WSL2 GDB in NAT mode cannot reach; give RPCS3 an
address the other host can reach with `GDB_SERVER` / `-GdbServer`, and only on a
network you trust (the GDB protocol has no authentication).

When using WSL GDB with a Windows-generated command file, copy the `.gdbinit`
and change its `file` path from `C:/...` to `/mnt/c/...`, and its `target remote`
address to the Windows host address. Add `set substitute-path C:/ /mnt/c/` so
GDB can find the Windows-built application's source. Keep the release-wait
assignment and breakpoint commands in that copy. A Windows path is not a
Linux filesystem path.

The acceptance run used a locally built Linux GDB 14.2. It does not validate the
released Linux GDB package or a Windows-hosted GDB executable. Compact PS3 `.opd`
descriptor handling and the release package's Python dependency remain separate
toolchain work; do not infer general function-call or descriptor support from
this source-breakpoint sample.
