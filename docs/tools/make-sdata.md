# make_sdata

`make_sdata` wraps a file into a developer SDATA file, the container that
`cellFsSdataOpen` reads, and can print or extract an existing one. A
developer SDATA file carries its data in plain form: there is no license, and
every digest and per-block hash field is zero. The command line follows
established PS3 SDK conventions, so existing build scripts that call a tool of
this name work unchanged.

Source: `tools/make-sdata` (Rust workspace).

## Install and locate

`scripts/build-sdk.sh` (through `scripts/install-host-tools.sh`) installs it as
`$PS3DEV/bin/make_sdata` and `$PS3DK/bin/make_sdata` on Linux. The Windows SDK
zip ships `%PS3DK%\bin\make_sdata.exe`. To build it alone:

```bash
cargo build --release --manifest-path tools/Cargo.toml -p make-sdata
# tools/target/release/make_sdata
```

## Usage

```
make_sdata [-options] <input file> <output file>
make_sdata -i <sdata file>
```

```cmd
:: wrap level1.dat with 8 KB blocks
make_sdata.exe -b 8 level1.dat level1.sdat

:: print the header of an SDATA file
make_sdata.exe -i level1.sdat

:: get the payload back
make_sdata.exe -x level1.sdat level1.dat
```

## Options

| Option | Meaning |
|---|---|
| `-h`, `--help` | Print the version line and usage, exit 0. |
| `-v`, `--version` | Print `make_sdata: version <v> (PS3 Custom Toolchain)`, exit 0. |
| `-p`, `--progress` | After writing, print `make_sdata: wrote <output> (<n> bytes)` to stderr. |
| `-b <KB>` | Block size in KB: 1, 2, 4, 8, 16 or 32. Default 16. Any other value is an error. |
| `-z`, `--compress` | Write the compressed layout. Every block is stored uncompressed (see below). |
| `--format2` | Write the format-2 layout (an older layout read by older system software). |
| `--format3` | Write the format-3 layout (an older layout). |
| `-x`, `--extract` | Treat the input as an SDATA file and write its data to the output file. |
| `-i`, `--info` | Print the header of one SDATA file; takes a single file argument. |

Without `--format2` / `--format3` the tool writes the current format
(version 4). `-x` and `-i` together is a usage error. Any other argument that
starts with `-` is rejected as an unknown option.

`-i` prints:

```
format type:  SDATA
file version: 4
file size:    <n> bytes
data length:  <n> bytes
block size:   16 KB (<n> blocks)
data type:    compressed        (only for the compressed layout)
created by:   <footer string>
```

### The compressed layout

`-z` writes the compressed layout with every block stored (its table entry
says "not compressed"). That is valid, because the format already stores any
block that does not shrink, so `-z` files read correctly but are not smaller
than the input. `-x` cannot read a block that is really compressed (one
written by another tool); it stops with an error naming the block.

## File layout

All integers are big-endian.

| Offset | Contents |
|---|---|
| `0x000` | 0x100-byte header: magic `NPD\0`, format version (2, 3 or 4) at 0x04, flags `0x81000000 | layout bits` at 0x80, block size at 0x84, data length (u64) at 0x88; everything else zero |
| `0x100` | Body. Versions 3/4: per block, 0x20 bytes of zero metadata then the block's data. Version 2: one 0x10-byte zero entry per block, then all the data. Compressed (any version): one 0x20-byte entry per block (0x10 zero bytes, u64 file offset, u32 stored length, u32 compressed flag), then the blocks, each padded to 16 bytes |
| end | 16-byte footer `SDATA 4.0.0.W` plus three zero bytes |

The file is padded with zeros to a multiple of 16 before the footer.

## Exit status and errors

| Status | When |
|---|---|
| 0 | Success. |
| 1 | Usage error, bad `-b` value, or an input/output file that cannot be read or written. |
| 4 | With `-i` or `-x`: the input is not an SDATA file this tool can read. |

Messages are `make_sdata: <path>: <reason>`. The reasons for status 4:

- `not an SDATA file`
- `unsupported SDATA format version <n>`
- `block size must be 1, 2, 4, 8, 16 or 32 KB`
- `layout flags <flags> do not match format version <n>`
- `the file is truncated or its block table is inconsistent`
- `block <n> is compressed; reading compressed blocks is not supported`

## Build integration

`cmake/ps3-self.cmake` provides:

```cmake
ps3_make_sdata(<output> <input> [BLOCK_KB <n>] [COMPRESS] [FORMAT 2|3])
```

It adds a custom command that runs `make_sdata` (found in `$PS3DEV/bin`,
`$PS3DK/bin` or the in-tree `tools/target/{release,debug}`). `<input>` is
relative to the source directory and `<output>` to the binary directory.
`BLOCK_KB` maps to `-b`, `COMPRESS` to `-z`, and `FORMAT 2|3` to
`--format2`/`--format3` (any other `FORMAT` value fails at configure time).
Something must depend on the output: embed it with `ps3_bin2s`, or list it in a
custom target that stages the application's files. Configure fails if the tool
is not installed.

`samples/lv2/hello-sdata` builds one payload in every layout (current format,
both old formats, 1 KB blocks, compressed), embeds them, writes each to
`/dev_hdd0/tmp`, opens it with `cellFsSdataOpen` and checks it reads back as
the payload.
