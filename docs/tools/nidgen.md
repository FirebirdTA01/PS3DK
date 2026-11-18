# nidgen

`nidgen` is the SDK's NID tool. It computes the NID (FNID) of a symbol name,
reads and checks the per-library YAML NID databases, and turns a database into
the assembly and the `lib<name>_stub.a` archive a PPU program links against
to import functions from a system or user PRX/SPRX module. It also renders the
export side (the `.lib.ent` table a module publishes) from the same YAML, so
one file describes both ends of an import.

Source: `tools/nidgen` (Rust workspace member).

## Install and locate

| Host | Location |
|---|---|
| Linux (from source) | `$PS3DEV/bin/nidgen`, mirrored to `$PS3DK/bin/nidgen` by `scripts/install-host-tools.sh` (run by `scripts/build-sdk.sh`) |
| Windows release zip | `%PS3DK%\bin\nidgen.exe` |
| In-tree build | `cargo build --release` in `tools/`, binary at `tools/target/release/nidgen` |

`nidgen --version` prints the SDK tools version; `nidgen --help` and
`nidgen <command> --help` print the option lists below.

## The NID algorithm

A NID is the first four bytes of `SHA-1(symbol_name || suffix)`, read as a
little-endian 32-bit integer, where `suffix` is the fixed 16-byte PS3 NID
suffix (see `tools/nidgen/src/nid.rs` and the PSDevWiki NID page). The NID of
a name never changes, so `nidgen nid` is all a module author needs to choose
the NID of a new export.

## Commands

### `nidgen nid <SYMBOL>...`

Prints the NID of each symbol, one per line, as `name<TAB>0x<8 hex digits>`.

```sh
$ nidgen nid prx_add cellNetCtlInit
prx_add	0xe1f4c294
cellNetCtlInit	0xbd5a59fc
```

At least one symbol is required.

### `nidgen stub --input <YAML> [-o <FILE>] [--abi ilp32|lp64]`

Renders a library's importer-side stub source (one assembly file holding the
`.lib.stub` record, the FNID table, the per-export trampolines and the
function descriptors) to `-o`/`--output`, or to stdout.

| Option | Meaning |
|---|---|
| `--input <YAML>` | Library YAML (required) |
| `-o`, `--output <FILE>` | Output file; stdout when omitted |
| `--abi <MODE>` | `ilp32` (default) or `lp64`. Any other value fails with `invalid --abi value` |

### `nidgen archive --input <YAML> --toolchain-bin <DIR> --out-dir <DIR> [options]`

Renders the stub source, assembles it and archives it: the one-step way to
make a linkable stub archive.

| Option | Meaning |
|---|---|
| `--input <YAML>` | Library YAML (required) |
| `--toolchain-bin <DIR>` | Directory holding the assembler and archiver, e.g. `$PS3DEV/ppu/bin` (required) |
| `--asm <NAME>` | Assembler program name (default `powerpc64-ps3-elf-as`) |
| `--ar <NAME>` | Archiver program name (default `powerpc64-ps3-elf-ar`) |
| `--out-dir <DIR>` | Output directory, created if missing (required) |
| `--abi <MODE>` | `ilp32` (default) or `lp64` |

If `<DIR>/<name>` does not exist but `<DIR>/<name>.exe` does, the `.exe` is
used, so the defaults work on Windows too.

Outputs in `--out-dir`, where `<base>` is the YAML's `archive_name` (or
`library` when that is absent):

- `<base>.s`: the rendered stub source
- `<base>.o`: the assembled object
- `lib<base>_stub.a`: the archive (any previous archive of that name is
  removed first, never appended to)

On success it prints `ok: <out-dir>/lib<base>_stub.a` to stderr. It fails if
the assembler or archiver cannot be run or exits non-zero.

```sh
nidgen archive --input exports.yml \
    --toolchain-bin "$PS3DEV/ppu/bin" --out-dir build/stubs
# -> build/stubs/libhello_prx_stub.a
```

Link it with `-L build/stubs -lhello_prx_stub`.

### `nidgen entgen --input <YAML> [-o <FILE>]`

The mirror of `stub`: renders the export table a module publishes, as an
`exports.S` written with the `<sys/prx_module.h>` macros
(`PRX_LIBRARY_BEGIN` / `PRX_EXPORT_FUNC` / `PRX_LIBRARY_END`), to `-o` or
stdout. NIDs are written as eight hex digits. A generated file and a
hand-written one are the same thing; see [prx-gen.md](prx-gen.md).

It refuses:

- a library with no exports (it would publish a record that exports
  nothing);
- an export with `kind: variable` (variable exports are not implemented by
  `<sys/prx_module.h>` yet);
- an export with `kind: tlsvar` (the loader ignores records that carry TLS
  variables).

### `nidgen verify <YAML>...`

Recomputes the NID of every export from its name and compares it with the
stored `nid`. For each file it prints `<library>: ok (<n> exports)` or one
`<library>: MISMATCH <name> stored=0x... computed=0x...` line per bad entry
(all on stderr), and exits 1 if any entry mismatched. A mismatch means the
name was mistyped or the stored NID is stale.

### `nidgen extract --input <ARCHIVE> [-o <YAML>]`

Reads an existing stub archive in the conventional layout (one object per
export, each carrying the export's NID in a marker section, plus a head
object for the library) and writes its library YAML to `-o`/`--output` or
stdout. With `-o` it reports `wrote <file> (<n> exports from <archive>)` on
stderr. Extracted entries have an empty `signature`, and `notes` records the
archive member the entry came from. It is how the databases under
`tools/nidgen/nids/extracted/` were produced.

## The library YAML

One file per library. The JSON Schema is `tools/nidgen/nids/schema.yaml`;
the fields `nidgen` reads are:

| Field | Required | Meaning |
|---|---|---|
| `library` | yes | Library name, written into the stub record (e.g. `cellNetCtl`, `sys_fs`) |
| `module` | yes | Module that resolves the imports at run time |
| `version` | no | Schema version, default `1` |
| `archive_name` | no | Archive basename: `lib<archive_name>_stub.a`. Defaults to `library` |
| `exports` | no | List of exported symbols (below) |
| `imports` | no | Symbols the library itself imports (rare) |

Each export:

| Field | Meaning |
|---|---|
| `name` | C symbol name (required) |
| `nid` | NID (required). `'0xbd5a59fc'`, `'bd5a59fc'` or a bare integer |
| `kind` | `function` (default), `variable` or `tlsvar` |
| `signature` | C prototype, documentation only |
| `aliases` | Extra C names bound to the same trampoline and NID (one extra descriptor each), e.g. legacy PSL1GHT names |
| `weak_aliases` | Aliases emitted weak, so an application definition may override them. Each must also be listed in `aliases`, or loading the YAML fails |
| `ordinal` | Optional ordinal, for libraries imported by ordinal |
| `notes` | Free text |
| `fnid` | Optional computed FNID, informational |
| `impl_status` | `unknown` (default), `stub`, `impl` or `verified`; read by [coverage-report](coverage-report.md) |

Minimal example (`samples/lv2/hello-sprx-export/exports.yml`):

```yaml
library: hello_prx
module: hello_prx
archive_name: hello_prx
version: 1
exports:
  - name: prx_add
    nid: '0xe1f4c294'
    signature: 'int prx_add(int lhs, int rhs);'
```

The databases shipped in the tree:

- `tools/nidgen/nids/*.yaml`: hand-curated databases with signatures and
  aliases (e.g. `sys_lv2.yaml`, `cellFs.yaml`);
- `tools/nidgen/nids/extracted/*.yaml`: one database per system stub
  library, generated by `nidgen extract`;
- `tools/nidgen/nids/aliases.yaml`: legacy-name aliases used by
  `coverage-report`.

YAML files contain names, NIDs and paraphrased signatures only.

## Build integration

### Installed system stub archives

`scripts/build-cell-stub-archives.sh` builds `nidgen` and runs
`nidgen archive` over its list of library YAMLs, twice: once with
`--abi ilp32`, installed into `$PS3DK/ppu/lib/`, and once with `--abi lp64`,
installed into `$PS3DK/ppu/lib/lp64/` where GCC's multilib search finds it
for `-mlp64` links. For a few libraries it appends C compatibility objects to
the archive and installs a legacy PSL1GHT archive name as a link to it.
Programs then link with `-l<archive_name>_stub`. It is part of the normal
build order (after `build-sdk.sh --headers-only`).

### CMake: importing from your own module

`ps3_prx_stub_library` (in `cmake/ps3-self.cmake`) wraps `nidgen archive`
into an imported static library target:

```cmake
include(ps3-self)
ps3_prx_stub_library(mymod_stub
    EXPORTS ../mymod/exports.yml      # required
    ARCHIVE_NAME mymod                # default: target name minus a trailing _stub
    ABI ilp32)                        # default: lp64 under the LP64 toolchain file, else ilp32
target_link_libraries(app PRIVATE mymod_stub rt lv2)
```

The archive is built at `<binary dir>/<target>/lib<ARCHIVE_NAME>_stub.a` with
the assembler and archiver next to the configured C compiler. Configure-time
errors: `EXPORTS is required`, `exports YAML not found`, `target ... already
exists`, and `required host tool 'nidgen' not found` (searched in
`$PS3DEV/bin` and `$PS3DK/bin`).

`ps3_prx_export_roundtrip` runs `nidgen entgen` on both the source YAML and
the YAML read back from a built module; see [prx-gen.md](prx-gen.md).

## See also

- [prx-gen.md](prx-gen.md): building PRX modules and the export round trip
- [coverage-report.md](coverage-report.md): coverage from the same databases
- `tools/nidgen/nids/README.md`: database conventions
