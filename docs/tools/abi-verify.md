# abi-verify

`abi-verify` is a structural checker for PPU ELF files. It reads a PPU
object, an archive member or a linked executable, summarises the parts of the
file the CellOS Lv-2 ABI constrains (ELF identity, section layout, `.opd`
function descriptors, the process parameter section, `.toc` relocations and
import-stub trampolines) as JSON, and checks them against the invariants in
[docs/abi/cellos-lv2-abi-spec.md](../abi/cellos-lv2-abi-spec.md).

Use it after changing anything that shapes PPU output (GCC or binutils
patches, CRT objects, linker scripts, post-link steps or stub generation) to
catch ABI drift before a binary reaches an emulator or a console. It does not
run code and does not replace a run on RPCS3 or hardware.

## Install and locate

`abi-verify` is a binary of the Rust workspace in `tools/` (crate
`tools/abi-verify`, which is also a library, `abi_verify`).

| Host | Location |
|---|---|
| Linux (from source) | `$PS3DEV/bin/abi-verify`, mirrored to `$PS3DK/bin/abi-verify` by `scripts/install-host-tools.sh` (run by `scripts/build-sdk.sh` unless `--no-host-tools`) |
| Windows release zip | `%PS3DK%\bin\abi-verify.exe` |
| Workspace build | `cd tools && cargo build --release -p abi-verify` puts it in `tools/target/release/` (Rust 1.88 or newer) |

`abi-verify --version` prints the tools version (`tools/VERSION`).

## Usage

```bash
# Check a linked executable or an object: prints PASS/FAIL lines
abi-verify check hello.elf
abi-verify check build/main.o

# JSON manifest of a file
abi-verify manifest hello.elf > hello.manifest.json

# JSON manifest of one member of an archive
powerpc64-ps3-elf-ar t libfoo.a            # list member names
abi-verify archive-member libfoo.a foo.o

# Fixed path field, so manifests can be stored and diffed as fixtures
abi-verify manifest build/main.o --label main.o > fixtures/main.o.json
```

Check the unstripped link output when you want the `.toc` and stub checks on
a linked file: they read relocation sections, which a plain link without
`-Wl,-q` (or a stripped file) does not keep. Objects (`.o`) always carry
them.

## Subcommands and options

| Command | Arguments and options | What it does |
|---|---|---|
| `manifest <PATH>` | `--label <TEXT>`: replace the `path` field in the output | Prints the JSON manifest of an ELF file to stdout. |
| `check <PATH>` | | Builds the manifest and runs the invariant checks; prints one line per result. |
| `archive-member <ARCHIVE> <MEMBER>` | `--label <TEXT>`: replace the `path` field | Finds the member whose archive name is exactly `<MEMBER>` and prints its JSON manifest. The default `path` is `<ARCHIVE>(<MEMBER>)`. |
| `help`, `--help`, `--version` | | Usage and version. |

`manifest` and `check` take an ELF file, not an archive; use
`archive-member` for a member of a `.a`. `archive-member` emits a manifest
only (there is no `check` form for an archive member; extract the member
with `powerpc64-ps3-elf-ar x` and run `check` on it).

## What is checked

Every file must be a big-endian ELF64 PowerPC64 file; anything else is an
error before any check runs. `check` then reports:

| Area | PASS when | Notes |
|---|---|---|
| ELF class, byte order, machine | ELF64, big-endian, PowerPC64 | |
| OS/ABI byte | `0x66` (CellOS Lv-2) | A relocatable object (`REL`) may also carry `0x00`. |
| `e_flags` | `0x00000000` | |
| `.opd` descriptors | entry size 8 (compact Lv-2 descriptor); each descriptor's first relocation is `R_PPC64_ADDR32` and it carries an `R_PPC64_TLSGD` tail relocation | An empty `.opd` passes as a stub-only object. The stride is inferred from relocation offsets. Checked only when `.opd` exists. |
| `.sys_proc_prx_param` | magic `0x1b434cec`, size `0x40`, the word at offset `0x20` is `0x01010000` | A zero at `0x20` stalls the loader before the entry point. Checked only when the section exists. |
| `.toc` data model | ILP32: only `R_PPC64_ADDR32`, slot stride 4. LP64: only `R_PPC64_ADDR64`, stride 8. | The model is inferred from the relocations (any `ADDR64` with no `ADDR32` means LP64); a mix of both fails. Checked only when `.toc` has `ADDR32`/`ADDR64` relocations. |
| `.sceStub.text` trampolines | no `stwu` (no stack frame) anywhere in the section; every trampoline decodes to the `bctr` tail-call shape (current stub archives, both data models) or the older ILP32 wrapping `bctrl` shape, which is accepted only under the ILP32 `.toc` data model | Unknown shapes fail with the word-level reasons (unexpected `mflr`/`mtlr`/`bctrl`, a stack-frame update, a missing or mismatched word). Checked only when the section exists. |

The checker covers the sections above. It does not list every relocation in
the file or check relocation types elsewhere.

## Output

`manifest` and `archive-member` print pretty JSON:

| Field | Content |
|---|---|
| `path` | Input path, `<archive>(<member>)`, or the `--label` text |
| `elf` | `class`, `endian`, `machine`, `os_abi`, `abi_version`, `elf_type` (`REL`, `EXEC`, `DYN`, ...), `flags` |
| `sections` | Every section: `name`, `sh_type`, `size`, `addralign`, `entsize`, `flags` |
| `opd` | When present: `entry_size`, `entries` and each descriptor's offset and relocations |
| `sys_proc_prx_param` | When present: `size`, `magic`, `version`, `sdk_version`, `abi_version`, relocations |
| `toc_check` | When computed: inferred `data_model`, `stride` and relocation counts |
| `stub_text_check` | When present: `stwu` offsets and each trampoline's name, offset, size, shape and failures |

`check` prints `  PASS  <text>` lines, then `  FAIL  <text>` lines.

## Exit status

| Code | Meaning |
|---|---|
| 0 | `manifest`/`archive-member` succeeded, or `check` found no failures |
| 1 | `check` reported at least one `FAIL` |
| 2 | Error, printed as `abi-verify: <message>`: unreadable file, not a big-endian ELF64 (`expected big-endian ELF`, `expected EM_PPC64, got 0x..`), a `.sys_proc_prx_param` smaller than 16 bytes, or `member <name> not found in archive` |

Usage errors (unknown subcommand or option) are reported by the argument
parser with its own exit code, 2.

## Build integration

Nothing in the SDK build, the CMake helpers or CI runs `abi-verify`
automatically; it is a developer check. The ABI documents treat it as the
first check after a toolchain or runtime change: a conformant sample links
with zero warnings and `abi-verify check` prints only PASS lines (see
[docs/abi/cellos-lv2-abi-spec.md](../abi/cellos-lv2-abi-spec.md) and
[docs/abi/toolchain-architecture.md](../abi/toolchain-architecture.md)).
The `manifest --label` form exists so manifests can be committed as fixtures
and compared with `diff` after a change.
