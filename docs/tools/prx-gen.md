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
relocation segment. Every original byte keeps its file offset.

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
-Wl,-q -Wl,-T,$PS3DK/ppu/lib/lv2-prx.ld` plus `lv2-prx-crt.o`; after the
link, `prx-gen build` writes the module and `prx-gen check` validates it.
`ps3_add_sprx` is an alias for `ps3_add_prx`.

Configure-time errors: target does not exist, `prx-gen` not found in
`$PS3DEV/bin` / `$PS3DK/bin`, `SIGN` without `make_sprx` or `fself`, or a
missing PRX runtime file (`lv2-prx.specs`, `lv2-prx.ld`, `lv2-prx-crt.o`
under `$PS3DK/ppu/lib`). Unrecognised arguments are warned about and ignored.

### Outside CMake

```sh
powerpc64-ps3-elf-gcc -specs="$PS3DK/ppu/lib/lv2-prx.specs" -nostartfiles \
    -Wl,-q -Wl,--no-warn-rwx-segments -Wl,-T,"$PS3DK/ppu/lib/lv2-prx.ld" \
    mymod.o exports.o "$PS3DK/ppu/lib/lv2-prx-crt.o" -o mymod.elf
prx-gen build mymod.elf -o mymod.prx --name mymod [--version 1.0] [--attributes 0x0000]
prx-gen check mymod.prx
make_sprx mymod.prx mymod.sprx          # optional, signed container
fself     mymod.prx mymod.fake.sprx     # optional, fake-signed container
```

Run `build` on the **unstripped** link output; `--name` is at most 27
characters (the module-info field width).

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

## See also

- [docs/design/sprx-generation.md](../design/sprx-generation.md): format and design notes
- [docs/abi/cellos-lv2-abi-spec.md](../abi/cellos-lv2-abi-spec.md) section 8:
  the relocatable module layout and link recipe
- [nidgen.md](nidgen.md): NIDs, export YAML, stub archives
- [sprx-linker.md](sprx-linker.md): the post-link step for executables
