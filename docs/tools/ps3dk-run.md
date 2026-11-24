# ps3dk-run: run a SELF on a console over the network

`ps3dk-run` sends a SELF from the PC to a PS3, starts it, streams its
`stdout` / `stderr` back, and reports how it ended. It has four parts, all
under `tools/ps3dk-run/`:

| Part | Where | What it is |
|---|---|---|
| Loader | `loader/` | `ps3dk-loader`, installed on the console as the PKG `PSDKLOADR`. Listens on TCP 4299, receives a SELF and its side files, and starts it. |
| Hook | `hook/` | `ps3dk_run_hook.c`, linked into the application you run. Calls back to the PC, forwards output and the exit status, and relaunches the loader when the application ends. |
| Host tool | `host/` | `ps3dk-run`, a Rust program in the `tools/` workspace. Builds and installs with the other host tools. |
| Test child | `child/` | `ps3dk-run-child`, the loader's acceptance test application. |

`common/` holds the wire protocol (`ps3dk_run_proto.h`) and the SHA-256,
HMAC-SHA-256 and socket code that the loader and the hook share.

**Trusted LAN only.** Every connection is authenticated and every frame
carries an HMAC tag, so a host without the pairing key cannot start
anything or forge results. Nothing is encrypted, though: the SELF, side
files and output cross the network in clear.

## Build

```bash
source ./scripts/env.sh
tools/ps3dk-run/build.sh            # loader PKG + test child, into $PS3_BUILD_ROOT/ps3dk-run
cargo build --release -p ps3dk-run --manifest-path tools/Cargo.toml   # host tool alone
```

`build.sh` configures `tools/ps3dk-run/CMakeLists.txt` with the installed
SDK's PPU toolchain file. `scripts/build-sdk.sh` builds the host tool along
with the other Rust tools; `ps3dk-run --version` prints the SDK version.

The loader build writes these files under `loader/`:

- `ps3dk-loader.self`, the loader as a plain-signed SELF;
- `ps3dk-loader.pkg` and `ps3dk-loader.gnpdrm.pkg` ([pkg.md](pkg.md));
- `pkg_overlay/USRDIR/RELOAD.SELF`, a copy of the `.self` that is packed into the PKG.

## Install the loader

1. Copy `ps3dk-loader.pkg` to a USB stick. On a console running PS3HEN, use
   **Package Manager → Install Package Files**. See [pkg.md](pkg.md) for
   which of the two `.pkg` files your firmware accepts.
2. Start **PS3DK Loader** from the XMB (Game column). The screen shows the
   console's IP address and port 4299, the key fingerprint and whether the
   file service is on.

The PKG installs to `/dev_hdd0/game/PSDKLOADR/USRDIR/`:

| Path | What it is |
|---|---|
| `EBOOT.BIN` | The loader as the XMB starts it (NPDRM-signed). |
| `RELOAD.SELF` | The same loader, plain-signed. After each run the hook relaunches this file; relaunching the NPDRM `EBOOT.BIN` from a running application dropped the console to the XMB. The hook falls back to `EBOOT.BIN` only when `RELOAD.SELF` is missing. |
| `runs/<run_id>/` | Where each run's SELF and side files are staged. The loader empties `runs/` every time it starts. |
| `state/` | The pairing key, the current run key and the file-service switch. No file-service root contains it, so it is never served. |

## Pair the PC and the console

The PC and the loader share one 32-byte key. It is generated on the PC and
carried to the console on a USB stick; it never crosses the network.

```bash
ps3dk-run keygen <dir>
```

`keygen` writes two copies of a new key:

- `<dir>/PS3DKRUN.KEY`, for the USB stick;
- the host copy, at `%APPDATA%\ps3dk-run\default.key` on Windows, or
  `$XDG_CONFIG_HOME/ps3dk-run/default.key` (by default
  `~/.config/ps3dk-run/default.key`) elsewhere.

It refuses to replace an existing host key, and it prints the key's
fingerprint (the first 16 hex digits of the key's SHA-256).

To pair:

1. Copy `PS3DKRUN.KEY` to the root of a USB stick and plug the stick into
   the console.
2. Press **TRIANGLE** on the loader screen. The loader reads the key from
   `/dev_usb000` or `/dev_usb001` and stores it in `state/key`.
3. Check that the fingerprint it shows matches the one `keygen` printed.
4. Delete the file from the stick.

To unpair or switch keys:

- **SQUARE** on the loader revokes the key: every session is rejected until a key is imported again.
- `ps3dk-run unpair` deletes the host copy.
- `--key <path>` (first argument) uses a different host key, for example one key per console.

**How authentication works:**

- **Session.** Each connection opens with a mutual challenge-response
  (CHALLENGE / AUTH / AUTH_OK) under the pairing key, which yields a session
  key. Every later frame is tagged with HMAC(session key, direction |
  sequence | type | length | payload), so a replayed, reordered or altered
  frame is rejected.
- **Run.** Each run gets a run key, HMAC(session key, "ps3dk-run/2 run" |
  run id). The loader writes it to `state/run.key` before starting the
  application, and the hook reads it from there; it never goes in `argv`.
- **Callback.** The hook's callback, the watchdog's TIMEOUT and the
  relaunched loader's READY are all tagged with the run key. The host drops
  untagged or wrongly tagged callback frames and closes their connection.

## Launch

```bash
ps3dk-run <console-ip> app.self [options] [-- args...]
```

| Option | Meaning |
|---|---|
| `--file <local>=<remote>` | Side file, staged at `<remote>` relative to the run directory (repeatable). |
| `--callback-host <ipv4>` | Address the console calls back to. The default is this host's end of the upload connection, which is wrong when NAT sits between the PC and the console. |
| `--callback-port <p>` | Port the host listens on for the callback (default 4300). The PC firewall must let the console reach it. |
| `--timeout <s>` | Watchdog timeout in seconds (0, the default, means no watchdog). See [Watchdog and rescue](#watchdog-and-rescue). |
| `--repeat <n>` | Run the same upload `n` times, each after the loader's READY. |
| `--outcome-json <path>` | Write the outcome records as a JSON array. |
| `-- args...` | Arguments for the application, which receives them as `argv[1..]`. |

**The sequence of a run:**

1. The host uploads the SELF and its side files to `runs/<run_id>/`.
2. The loader checks the SELF header (while it can still reply), writes the
   run key, and replaces itself with the application through `exitspawn`.
   The arguments it passes are the user arguments followed by a
   `--ps3dk-run=<run_id>:<callback-ip>:<port>:<timeout>` marker.
3. The hook finds the marker, removes it from `argv`, connects back and
   sends HELLO.
4. Output to fd 1 and fd 2 is forwarded to the host and still reaches the
   console TTY.
5. When the process exits, after `atexit` handlers, static destructors and
   stdio cleanup, the hook sends EXIT with the status and relaunches the
   loader.
6. The relaunched loader sends READY, and the host returns.

**Exit status of `ps3dk-run`:**

| Status | Meaning |
|---|---|
| 0–255 | EXITED: the application's own exit status, `& 255`. |
| 124 | TIMEOUT. |
| 125 | NO_HOOK: no HELLO arrived. |
| 127 | START_FAILED. |
| 3 | REJECTED / FILE_ERR. |
| 4 | BUSY. |
| 5 | Authentication failure. |
| 126 | Any other failure. |
| 2 | Usage error. |

Scripts that need more than the status should read `--outcome-json`.

**START_FAILED** means the hook sent HELLO and got an answer but then could
not start (its watchdog or output-sender thread could not be created), so
it relaunched the loader instead of running the application. A hook that
fails before HELLO can only relaunch; that run reports NO_HOOK, or
DISCONNECTED if the loader's READY arrives first.

## Files: the run directory and dev_hdd0

The application's own files go with the run:

```bash
ps3dk-run 192.168.1.41 game.self --file assets/level1.bin=data/level1.bin
```

- `--file` stages the file at `/dev_hdd0/game/PSDKLOADR/USRDIR/runs/<run_id>/data/level1.bin`,
  next to the SELF. `argv[0]` is the SELF's path, so the application finds
  its data beside `argv[0]`.
- Remote paths must be relative and printable ASCII, with no `.`, `..`,
  empty component, backslash or colon, and no duplicates.
- A run is limited to 256 files, 64 MiB per file and 256 MiB in total; a
  path is at most 200 bytes.
- The next loader start deletes the whole run directory. For data that must
  outlive a run, use the file service.

The file service reaches `/dev_hdd0/tmp` and nothing else:

```bash
ps3dk-run ls  <console> [dir]
ps3dk-run get <console> <remote> <local>
ps3dk-run put <console> <local> <remote> [--replace]
ps3dk-run rm  <console> <remote>
ps3dk-run files-off <console>
```

- It is **off** until you press **CIRCLE** on the loader. `files-off`, or
  CIRCLE again, turns it off.
- Its state survives a run (it is restored when the hook relaunches the
  loader) but not a cold start from the XMB.
- `--replace` never loses the old file: the old file is moved aside, the
  new one put in place, and the old copy deleted; a failure moves the old
  file back.
- The loader's `state/` directory is outside the root and is never served.
- lv2's `sys_fs_link` makes a hard link on `dev_hdd0`. The root is a trust
  boundary against a paired host's mistakes, not containment against a
  hostile application on the console.

## The hook

The loader can start any SELF, but only an application with the hook linked
in can talk back. The hook is `hook/ps3dk_run_hook.c` plus `common/`, and
its entry point is:

```c
int ps3dk_run_hook_start(int *argc, const char ***argv);   /* first statement of main */
```

**Without the `--ps3dk-run=` marker the hook is inert.** It starts no
network or threads, redirects nothing and leaves the exit unchanged, so the
same SELF also boots from the XMB or in RPCS3. With the marker, it removes
the marker from `argv` and does the work described under [Launch](#launch).

`ps3dk_run_hook_start` returns:

- 1 when the hook is active;
- 0 when it is inert;
- -1 when the marker is malformed (the hook stays inert).

Any failure after the marker parses relaunches the loader and does not
return.

### Building an application with the hook

**Calling it yourself.** Add the hook sources and call it first in `main`:

```cmake
set(PRN ${PS3DK_REPO}/tools/ps3dk-run)
target_sources(app PRIVATE ${PRN}/hook/ps3dk_run_hook.c ${PRN}/common/prn_net.c
                           ${PRN}/common/prn_sha256.c ${PRN}/common/prn_hmac.c)
target_include_directories(app PRIVATE ${PRN}/hook)
target_link_libraries(app PRIVATE net sysmodule_stub)
```

```c
#include "ps3dk_run_hook.h"
int main(int argc, const char *argv[])
{
    ps3dk_run_hook_start(&argc, &argv);
    ...
}
```

**Leaving an unmodified project alone.** Inject the hook at configure time:

```bash
cmake -S app -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PS3DK/cmake/ps3-ppu-toolchain.cmake \
      -DCMAKE_PROJECT_INCLUDE=<repo>/tests/hardware/psgl-sample-boot/run-hook-inject.cmake \
      [-DPS3DK_RUN_HOOK_DIR=<dir with hook/ and common/>] [-DPS3DK_RUN_HOOK_TARGETS="app"]
```

- Every executable target gets its own sources compiled with `main` renamed
  to `ps3dk_sample_main`.
- A generated `main` (`hook_main.c`) starts the hook and calls
  `ps3dk_sample_main(argc, argv)` with the marker already removed. A C++
  `int main()` with no parameters is detected and called without arguments.
- The hook objects are compiled without `-finstrument-functions`, so
  profiler instrumentation never covers the loader's code.
- `PS3DK_RUN_HOOK_DIR` defaults to this checkout's `tools/ps3dk-run`.
- Use `PS3DK_RUN_HOOK_TARGETS` when the project also builds PRX modules:
  `ps3_add_prx` targets are executables to CMake, and a module must not get
  the hook or a `main`.

### Checking that a SELF carries the hook

A SELF is compressed and its ELF usually stripped, so you cannot see the
hook in the output file. `hook-check` inspects the build instead.

```bash
tools/ps3dk-run/hook-check.py <build-dir> <target> <main-object> [--module <prx-target>]
#  e.g. hook-check.py build app source/main.c.obj --module mymodule.sprx
```

On Windows with a Ninja build there is also
`tools/ps3dk-run/hook-check.ps1 <build-dir> <target> <main-object> [<module>]`.

Both check five things:

1. The link line holds the hook object and the injected `main`.
2. `nm` shows that `ps3dk_run_hook_start` is defined, that the injected
   `main` calls `ps3dk_sample_main`, and that the application's own main
   object defines it. PPC64 functions show as `.opd` descriptors (type D)
   C++ names are mangled, and a C++ wrapper's references are weak (type
   w), so all of these are accepted.
3. The ELF holds the hook's strings `ps3dk-run-watchdog` and `ps3dk-run=`.
4. With a module named, the module has neither string and no hook object.
5. The build was configured with the injection, and the SELF is not older
   than its ELF.

Each check prints its own line. The script then prints the SELF's SHA-256
and ends with `HOOK PRESENT` (exit 0) or `HOOK CHECK FAILED` (exit 1).

### What happens without the hook

The loader still starts the SELF, and the application runs normally on the
console. But:

- nothing calls back, so the host waits for HELLO and reports **NO_HOOK**
  (status 125);
- no output is captured;
- when the application exits, nothing relaunches the loader: the console
  returns to the XMB, and the loader must be started again from there
  before the next run.

An application built before the argv fix in its startup code never sees the
marker under ILP32, so it behaves exactly like one without the hook. If you
see NO_HOOK, run `hook-check` first.

## Watchdog and rescue

`--timeout <s>` gives the run a watchdog: a high-priority thread in the
hook, started before the application's code runs. If the application has
not exited after `<s>` seconds, the watchdog:

1. sends a TIMEOUT frame (tagged with the run key) on a connection of its own;
2. relaunches the loader with `exitspawn`.

The host reports TIMEOUT (status 124), and the loader's READY confirms the
console is back. The hook's exit path and the watchdog race through a single
latch, so a run has exactly one outcome and one relaunch.

The watchdog rescues an application that is busy or blocked on the CPU.
**It does not rescue a GPU wedge.** In one hardware hang the watchdog fired
and the loader relaunched, but the new loader stopped before its GCM
initialisation finished, apparently because the RSX was still wedged, and
the console needed a power cycle. There is no network "rescue" command, and
no controller combination that kills a run.

Without `--timeout`, a hung application stays hung until the console is
power-cycled (and, on PS3HEN, HEN is re-enabled).

## Known limits

- **Intermittent launch hang (open).** Twice on 2026-10-17 the console hung
  while a run was starting:
  - once when the loader itself started from the XMB;
  - once on the second launch of a repeated protocol-2 test child, with no
    HELLO.

  Both times the screen stayed black, the PS button was dead and a power
  cycle was needed. Instrumented reruns were clean, and the cause is not
  known. Expect it to be rare but possible, and do not leave a console
  running unattended on the assumption that every run comes back.
- **GPU wedges** need a power cycle (see [Watchdog and rescue](#watchdog-and-rescue)).
- **No confidentiality.** Use it on a LAN you trust.
- **Output before `main` is not captured.** The hook starts in `main`, so
  output from static constructors goes only to the TTY.
- **Lossy output under load.** Output goes through a bounded queue. When the
  queue is full or the connection is lost, output is dropped and counted;
  the application is never blocked.
- **Argument limits:** 64 arguments and 4096 bytes of `argv` in total.
- **One run at a time:** a second host gets BUSY.
- **The console must be able to reach the PC** at the callback address. With
  NAT in between, pass `--callback-host`.
- **Loader version.** The loader shows no version string. A console
  installed from an earlier build of this tree runs the protocol-2 loader
  without START_FAILED reporting. Reinstall the PKG to get the current one;
  the wire protocol is the same.

## Tests

- `cargo test -p ps3dk-run` (part of `cargo test --workspace`) covers the
  host tool's protocol, HMAC and SHA-256.
- `tests/host/ps3dk-run-crypto-test.py` builds the console-side C SHA-256 /
  HMAC with the host compiler and checks them against Python's `hashlib` /
  `hmac` across the padding and key-length edges.
- `tests/host/run-hook-inject-argv-test.py` builds C and C++ applications,
  with and without `argc`/`argv`, through the injection, and checks that
  `main` receives the arguments without the marker. It also runs
  `hook-check.py` against those builds.
- `tests/host/run-hook-inject-test.py` checks that the hook and loader
  objects stay out of profiler instrumentation.

All four run in CI. The console-side acceptance test is `child/`, run
through `ps3dk-run` against a real console.
