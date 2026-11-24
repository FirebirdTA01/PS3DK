# prx-gen

`prx-gen` turns a linked PPU ELF into a loadable Lv-2 relocatable module
(`.prx`), validates a built module against the loader's requirements, and
reads a module's export table back out as a [nidgen](nidgen.md) library
YAML. With `ps3_add_prx` in CMake it is how this SDK builds PRX and SPRX
modules that other programs load and call.

Source: `tools/prx-gen` (Rust workspace member).

## Install and locate

| Host | Location |
|---|---|
| Linux (from source) | `$PS3DEV/bin/prx-gen`, mirrored to `$PS3DK/bin/prx-gen` by `scripts/install-host-tools.sh` (run by `scripts/build-sdk.sh`) |
| Windows release zip | `%PS3DK%\bin\prx-gen.exe` |
| In-tree build | `cargo build --release` in `tools/`, binary at `tools/target/release/prx-gen` |

`prx-gen --version` prints the SDK tools version.

## Commands

### `prx-gen build <INPUT> -o <OUTPUT> [options]`

Converts the `-Wl,-q` link output into a PRX. It stamps the PRX ELF identity
(`EI_OSABI 0x66`, `e_type 0xFFA4`, `e_entry 0`), points the first program
header at the module-info block, writes the module name, version and
attributes into that block, fills in the import counts of any `.lib.stub`
records, and converts the retained ELF relocations into the module's
relocation segment. It sets the SPU permission bits on every `PT_LOAD`
(SPU read on all, SPU write on writable ones, so `lv2-prx.ld`'s segments
come out as `0x00400005` and `0x00600006`) and clears `p_paddr` on all but
the first. Every original byte keeps its file offset.

A module is linked as two segments, the layout retail Lv-2 needs (issue
#137): code and the read-only tables the loader walks in an RX segment, and
the TOC, `.opd`, `.rodata`, `.data` and `.bss` in an RW segment. Each
segment is placed at its own base, so a relocation is dropped only when its
two ends move together:

| Relocation | Dropped when | Otherwise |
|---|---|---|
| `REL24`, `REL14*` (branches) | the target decoded from the linked instruction is in the branch's segment. A `bl` names the callee's descriptor in `.opd`, which is in the data segment, so the symbol is not the target | refused |
| `TOC16*`, `GOT16*` | the target is in the TOC's segment, wherever the instruction is | refused |
| `REL64` | site and target share a segment | emitted as loader type 44 |
| `REL32`, `REL16*` | site and target share a segment | refused |
| `SECTOFF*`, `NONE` | always | |

Absolute forms are emitted against the segment of the symbol's section;
`.TOC.` (which lies past the image) is bound to the segment holding `.got`.

| Option | Meaning |
|---|---|
| `<INPUT>` | The linked ELF. Must be **unstripped**: the retained relocations reference `.symtab` |
| `-o`, `--output <FILE>` | Output module (required) |
| `--name <NAME>` | Module name, at most 27 bytes. Optional when the source declares `SYS_MODULE_INFO`; must match it when both are given |
| `--version <M.m>` | Module version as `major.minor` (each 0-255). Default `1.0` unless `SYS_MODULE_INFO` declares one |
| `--attributes <N>` | 16-bit module attributes word, decimal or `0x` hex. Default `0` unless `SYS_MODULE_INFO` declares one |
| `--allow-unrelocated-module-info` | Build even if fewer than five module-info pointer fields carry relocations (prints a warning instead of failing). Almost always wrong |
| `-q`, `--quiet` | No summary |

Without `--quiet` it prints a summary: module name, version and attributes;
the module-info address and the `PHDR[0].p_paddr` written; relocations
emitted and dropped; how many module-info fields are relocated; and how many
`.lib.stub` import records had their count filled in.

Errors it reports (and refuses to build):

- the input is already a PRX (`e_type = 0xFFA4`): run it on the linker
  output, not on its own result;
- the input is not a linked executable, has no program headers, or its first
  program header is not `PT_LOAD`;
- neither the module-info section nor its symbol exists: link against the
  PRX crt (`lv2-prx-crt.o`);
- a `PT_LOAD` segment is both writable and executable (retail Lv-2 freezes
  a module that writes its own `.data`/`.bss` in one): link with the current
  `lv2-prx.ld`;
- a relocation cannot survive the two segments moving independently: a
  branch into another segment, a TOC-relative access to data outside the
  TOC's segment, or a PC-relative form other than `REL64` that crosses
  segments;
- the module-info block is outside the first `PT_LOAD` segment, or sits at
  its very start (a `p_paddr` of 0 means "no module info");
- no module name (pass `--name` or declare `SYS_MODULE_INFO`), a name of 27
  bytes or more, or a name, version or attributes value that disagrees with
  `SYS_MODULE_INFO`;
- a malformed `SYS_MODULE_INFO` record (declared more than once, wrong size,
  empty or unterminated name);
- fewer than 5 of the module-info pointer fields (`toc`, `exports_start`,
  `exports_end`, `imports_start`, `imports_end`) are relocated, which would
  load a module that walks garbage;
- a `version` not in `major.minor` form, or an `--attributes` value that does
  not fit 16 bits.

### `prx-gen check <INPUT>`

Validates a built module: ELF identity, segments, the module-info block and
its relocations, the relocation segment (only relocation types the loader
handles), and every export and import record. It prints a report and ends
with `OK`, or with each `ERROR:` line and an error count and a non-zero exit
(`<file> is not a loadable module`). Warnings (for example a non-zero
`e_entry`) do not fail it.

Keep `check` in the build. The failures it catches, such as an export record
with the wrong attributes or an unrelocated module-info pointer, produce a
module that loads cleanly and exports nothing, with no diagnostic in the
emulator log.

Three of its rules exist because the emulator accepts what retail Lv-2 does
not (issue #137); each is an error:

| Rule | On hardware without it |
|---|---|
| every `PT_LOAD` has the SPU read bit `0x00400000`, and a writable one the SPU write bit `0x00200000` | load fails with `0x80010007` |
| export (`.lib.ent`) records are `0x1c` bytes; import (`.lib.stub`) records `0x2c` | with two or more export records, `sys_prx_start_module` hangs |
| no `PT_LOAD` is both writable and executable | the first store to the module's `.data` or `.bss` freezes the process |

### `prx-gen exports <INPUT> [-o <YAML>] [--library <NAME>]`

Reads the module's export table and writes it as nidgen-schema YAML
(`library`, `module`, `exports: [{name, nid}]`) to `-o`/`--output` or stdout.

| Option | Meaning |
|---|---|
| `-o`, `--output <FILE>` | Output YAML; stdout when omitted |
| `--library <NAME>` | Emit only this library. Required when the module exports more than one |

Exports whose address has no symbol-table entry are written under
`nid_<hex>` placeholder names with a warning; run it on an unstripped module
to recover the real names. It fails if the module carries no module info,
exports nothing, has export records but no named library, or has a name that
would need YAML quoting.

## Building a PRX / SPRX module

A module is an ordinary CMake target plus a short export table; the importer
is an ordinary executable. Worked pair: `samples/lv2/hello-sprx-export` (the
module) and `samples/lv2/hello-sprx-import` (loads it, calls it, prints
`PRX_OK`).

### 1. The module

`CMakeLists.txt`:

```cmake
include(ps3-self)
add_executable(mymod source/mymod.c source/exports.S)
ps3_add_prx(mymod NAME mymod VERSION 1.0 EXPORTS exports.yml SIGN)
```

`source/exports.S` names what the module exports, with NIDs from
`nidgen nid <symbol>`:

```asm
#include <sys/prx_module.h>
PRX_LIBRARY_BEGIN(mymod, "mymod")
    PRX_EXPORT_FUNC(mymod_add, 0xe1f4c294)
PRX_LIBRARY_END(mymod)
```

`nidgen entgen --input exports.yml` generates this file from the YAML if you
prefer one source of truth.

`module_start(size_t args, void *argp)` and `module_stop` are optional: the
crt provides weak defaults that return `SYS_PRX_RESIDENT`. Define them in C
to run code at start and stop; a `module_start` that returns anything other
than `SYS_PRX_RESIDENT` (0) makes Lv-2 unload the module immediately.

Instead of `NAME`/`VERSION`/`ATTRIBUTES`, the module source can declare its
identity and entry points with `<sys/moduleexport.h>`:

```c
#include <sys/moduleexport.h>
SYS_MODULE_INFO(mymod, 0, 1, 0);       /* name, attributes, major, minor */
SYS_MODULE_START(mymod_start);
SYS_MODULE_STOP(mymod_stop);
```

`prx-gen build` copies those values into the module-info block; if both the
source and the CMake arguments give a value they must agree.
`SYS_MODULE_EXIT` and `SYS_MODULE_STOP_LEVEL` are not supported yet and fail
to compile. Variable and TLS exports are not supported.

### `ps3_add_prx` reference

```cmake
ps3_add_prx(<target>
    [NAME <name>] [VERSION <M.m>] [ATTRIBUTES <n>]
    [EXPORTS <exports.yml>]
    [OUTPUT <path> | OUTPUT_NAME <file> | OUTPUT_DIRECTORY <dir>]
    [SIGN])
```

| Argument | Meaning |
|---|---|
| `NAME`, `VERSION`, `ATTRIBUTES` | Passed to `prx-gen build` as `--name`, `--version`, `--attributes` when given (required `NAME` unless the source declares `SYS_MODULE_INFO`) |
| `EXPORTS` | The module's export YAML; recorded for `ps3_prx_export_roundtrip` |
| `OUTPUT` | Full output path (relative paths resolve against the binary dir) |
| `OUTPUT_NAME` | Output file name, default `<target>.prx` |
| `OUTPUT_DIRECTORY` | Output directory; default `PS3_SELF_OUTPUT_DIRECTORY`, else the current binary dir |
| `SIGN` | Also emit `<stem>.sprx` (with `make_sprx`) and `<stem>.fake.sprx` (with `fself`) beside the `.prx` |

The target is linked with `-specs=$PS3DK/ppu/lib/lv2-prx.specs -nostartfiles
-Wl,-q -Wl,-T,$PS3DK/ppu/lib/lv2-prx.ld` plus `lv2-prx-crt.o`, with
`-llv2prx` first among its libraries (see
[Threads in a module](#5-threads-in-a-module)); after the link, `prx-gen build` writes the module and `prx-gen check` validates it.
`ps3_add_sprx` is an alias for `ps3_add_prx`.

Configure-time errors: target does not exist, `prx-gen` not found in
`$PS3DEV/bin` / `$PS3DK/bin`, `SIGN` without `make_sprx` or `fself`, or a
missing PRX runtime file (`lv2-prx.specs`, `lv2-prx.ld`, `lv2-prx-crt.o`
under `$PS3DK/ppu/lib`). Unrecognised arguments are warned about and ignored.

### Outside CMake

```sh
powerpc64-ps3-elf-gcc -specs="$PS3DK/ppu/lib/lv2-prx.specs" -nostartfiles \
    -Wl,-q -Wl,-T,"$PS3DK/ppu/lib/lv2-prx.ld" \
    mymod.o exports.o "$PS3DK/ppu/lib/lv2-prx-crt.o" -o mymod.elf
prx-gen build mymod.elf -o mymod.prx --name mymod [--version 1.0] [--attributes 0x0000]
prx-gen check mymod.prx
make_sprx mymod.prx mymod.sprx          # optional, signed container
fself     mymod.prx mymod.fake.sprx     # optional, fake-signed container
```

Run `build` on the **unstripped** link output; `--name` is at most 27
characters (the module-info field width). `lv2-prx.specs` adds `-llv2prx`
ahead of the default libraries; a link that names `-lrt` itself must put
`-llv2prx` before it, or librt's thread pair is linked instead.

### 2. The importer

Link a stub archive generated from the same export list
(`exports.yml`, nidgen's schema: `library`, `module`, `exports: [{name, nid}]`):

```cmake
include(ps3-self)
ps3_prx_stub_library(mymod_stub EXPORTS ../mymod/exports.yml ARCHIVE_NAME mymod)
add_executable(app source/main.c)
target_link_libraries(app PRIVATE mymod_stub rt lv2)
ps3_add_self(app)
```

See [nidgen.md](nidgen.md#cmake-importing-from-your-own-module) for the
`ps3_prx_stub_library` options.

In `main.c`: `sysPrxLoadModule("/app_home/mymod.prx", 0, NULL)`, then
`sysPrxStartModule(id, args, argp, &modres, 0, NULL)` (the `modres` pointer
must be non-NULL). After start, calls to `mymod_add()` resolve through the
stub, and `argp` is a convenient way for `module_start` to hand back
pointers. Copy the `.prx` next to the booted executable so `/app_home/`
finds it.

### 3. Running on RPCS3

Boot the importer's raw `.elf` and the unsigned `.prx` loads as-is. Booting
the `.fake.self` requires the module's `.fake.sprx`; booting the real
`.self` requires the `.sprx`. The module's container follows the
executable's, which is why `SIGN` emits both.

### 4. Keeping export lists honest

`prx-gen exports mymod.prx` reads a built module's export table back out as
nidgen-schema YAML, and `ps3_prx_export_roundtrip(<target>)` wires that into
the build (it needs `ps3_add_prx(... EXPORTS <yml>)` first):

```cmake
ps3_add_prx(mymod NAME mymod EXPORTS exports.yml)
ps3_prx_export_roundtrip(mymod)
```

It adds a `<target>_export_roundtrip` target (part of `ALL`) that runs
`prx-gen exports` on the built module, renders both that YAML and the source
YAML through `nidgen entgen`, and compares the results. The library name and
the export names and NIDs in order must match; fields an export table cannot
carry (`archive_name`, signatures) are ignored. On a mismatch the build fails
with both tables printed, so a renamed export cannot silently strand its
importers.

### 5. Threads in a module

`sys_ppu_thread_create` and `sys_ppu_thread_exit` (`<sys/ppu_thread.h>`)
work in a module. `ps3_add_prx` and `lv2-prx.specs` link `liblv2prx.a` ahead
of librt, and that pair reads no TLS and allocates nothing; librt's own pair is TLS-based
and cannot link into a module. A module therefore has no thread_local and no
pthread API: it has no TLS of its own, and `prx-gen` refuses TLS relocations.

- `sys_ppu_thread_create` starts the entry through a small start record, so
  the full 64-bit argument arrives and a return from the entry ends the
  thread with exit code 0. The 16 records are a fixed pool, held only from
  the create until the new thread starts; with all of them held the call
  returns `EAGAIN` (`0x80010001`).
- `sys_ppu_thread_exit` ends a thread that has a TLS area through liblv2's
  exit, and one without (see below) through the bare exit system call (41).
  Either way the exit code reaches `sys_ppu_thread_join`.

### 6. VSH plugins

A VSH plugin is a module that a plugin loader (webMAN MOD's `/loadprx.ps3`
and `/unloadprx.ps3` pages, through Cobra) loads into the VSH process on a CFW or HEN console. Its `module_start` and
`module_stop` run on different kinds of thread depending on who loads it, and
a plugin has to handle both. `samples/sprx/vsh-plugin` is the worked
example; its loader runs the loader path below in a game process.

| | Loader path: `sys_prx_start_module` / `sys_prx_stop_module` in a game | Plugin loader path: Cobra in VSH |
|---|---|---|
| Thread | a normal thread: a TLS area and a caller to return to | made by the kernel: no TLS area, a 4 KiB stack, and a poisoned return address (`0xBADADD00`, reported by testers in issue #137) |
| Ending `module_start` / `module_stop` | return the result | end the thread with system call 41 (returning faults, and liblv2's `sys_ppu_thread_exit` reads TLS); its exit code is the result |
| Final stop report | liblv2's `sys_prx_stop_module` sends it after `module_stop` returns | `module_stop` must send it itself, or the module never reaches the stopped state and the unload waits for ever |
| `argp` | whatever the caller passes | none |

**The pattern.** `module_start` and `module_stop` do as little as possible
on the thread they are given:

1. `module_start` starts a worker with `sys_ppu_thread_create`. The worker is
   a normal thread (liblv2 gives it a TLS area and the stack size asked for)
   and does the plugin's job. Then `module_start` ends with
   `SYS_PRX_RESIDENT`: by returning on the loader path, with system call 41 on
   the plugin loader path.
2. `module_stop` tells the worker to stop, joins it (`sys_ppu_thread_join` is
   a bare system call, fine on either thread), and ends with
   `SYS_PRX_STOP_OK`. On the plugin loader path it first sends the final stop
   report, then ends with system call 41.

To tell the paths apart, the sample checks for a usable caller: a return
address that is not 0, is word-aligned and is not the poison value, and a
TLS area (r13 at least `0x7000`, as in `<sys/ppu_thread.h>`'s exit). Either
missing means the plugin loader path. Both checks read registers only.

**The final stop report** is system call 482 (`_sys_prx_stop_module`) on the
module's own id (system call 461, `_sys_prx_get_module_id_by_address`, with
any address inside the module) with flags 0 and a five-word option block:
size `0x28`, command 2, entry 0, result 0, entry 0. Command 2 with result 0
moves the module from stopping to stopped, which is what lets the unload
finish; result 1 means "cannot stop" and leaves it stopping. liblv2's
`sys_prx_stop_module` sends exactly this report after `module_stop` returns,
so a plugin must not send it on the loader path: the module would already be
stopped when liblv2's own report arrives. (Semantics as RPCS3 models them,
`rpcs3/Emu/Cell/lv2/sys_prx.cpp`, `_sys_prx_stop_module`; the plugin loader
sending only the first stop command is how Cobra's module code behaves, as
discussed in issue #137.)

**Known limits.**

- The plugin loader path is not emulated: RPCS3 has no VSH plugin loader, so
  that path is tested only on a console.
- The path check relies on the plugin loader's thread having neither a TLS
  area nor a normal return address. A loader that gave its thread one but
  not the other is still caught; one that gave it both but still did not
  finish the stop would not be.
- The plugin loader's thread has a 4 KiB stack. Keep `module_start` and
  `module_stop` small and leave the work, and anything that needs a stack
  or TLS (newlib, file I/O through stdio), to the worker.
- Write the module's own `.data` and `.bss` freely (the two-segment layout
  makes that work), but there is no thread_local or pthread API in a module.
- Under LP64, GCC puts a `switch` jump table in `.text` and reaches it
  through r2, so the access crosses from the data segment to the code
  segment and `prx-gen` refuses the module. Build LP64 module code with
  `-fno-jump-tables`; newlib's formatted I/O has such tables and cannot be
  linked into an LP64 module. ILP32 modules are not affected. VSH plugins
  are normally ILP32.

## See also

- [docs/design/sprx-generation.md](../design/sprx-generation.md): format and design notes
- [docs/abi/cellos-lv2-abi-spec.md](../abi/cellos-lv2-abi-spec.md) section 8:
  the relocatable module layout and link recipe
- [nidgen.md](nidgen.md): NIDs, export YAML, stub archives
- [sprx-linker.md](sprx-linker.md): the post-link step for executables
