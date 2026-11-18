# Installable packages: ps3_add_pkg, pkg, make_self_npdrm, package_finalize

A `.pkg` is the archive a PS3 installs from a USB stick or the network, and
the file you drop onto RPCS3 to install an app under a stable title id
(RPCS3 keys its shader cache and per-game data on that id rather than on a
`.self` path). The SDK builds packages with four host tools:

| Tool | Role | Origin |
|---|---|---|
| `make_self_npdrm` | Signs the stripped ELF as an NPDRM SELF, `USRDIR/EBOOT.BIN`, under the content id | PSL1GHT host tool |
| `sfo` | Generates `PARAM.SFO` from an XML template ([sfo.md](sfo.md)) | This repository (`tools/sfo`) |
| `pkg` | Packs the staging directory into a `.pkg` | `tools/sfo-pkg/pkg.c`, from PSL1GHT with local fixes |
| `package_finalize` | NPDRM-finalises a copy of the `.pkg` | PSL1GHT host tool |

From CMake you call none of them directly: `ps3_add_pkg()` runs the whole
sequence.

## Install and locate

| Host | Location |
|---|---|
| Windows release | `%PS3DK%\bin\pkg.exe`, `make_self_npdrm.exe`, `package_finalize.exe`, `sfo.exe`, plus `sfo.xml` and `ICON0.PNG` |
| Linux, from source | `$PS3DEV/bin/` (`pkg` is mirrored into `$PS3DK/bin/`) |

On Linux, `scripts/install-host-tools.sh` (run by `scripts/build-sdk.sh`)
builds `pkg` with `scripts/build-pkg-host.sh <output-dir>`, which first
compiles and runs a SHA-1 self-test (`tools/sfo-pkg/sha1-test.c`) and
refuses to build `pkg` if it fails. `HOST_CC` selects the host compiler
(default `cc`). `make_self_npdrm` and `package_finalize` come with the
PSL1GHT host tools (`scripts/build-psl1ght.sh`); the Windows release
cross-builds them in `scripts/build-host-tools-windows.sh`.

## ps3_add_pkg (CMake)

Any sample, or your own app, becomes an installable `.pkg` with one call
after `ps3_add_self`:

```cmake
include(ps3-self)
ps3_add_self(myapp)
ps3_add_pkg(myapp
    CONTENTID "UP0001-MYAPP0001_00-PS3DKSAMPLES0000")
```

Signature:

```
ps3_add_pkg(<target> CONTENTID <id>
            [TITLE <str>] [APPID <id>]
            [ICON <path>] [SFOXML <path>] [PKGFILES <dir>])
```

| Argument | Default | Meaning |
|---|---|---|
| `CONTENTID` | required | The 36-character content id, `XX0000-AAAAAAAAA_00-XXXXXXXXXXXXXXXX`. Characters 8 to 16 are the 9-character **title id**, which is what the console and RPCS3 key the install on. A length other than 36 is a configure-time warning (real hardware rejects the package). |
| `TITLE` | the target name | `TITLE` in `PARAM.SFO` |
| `APPID` | the title id inside `CONTENTID` | `TITLE_ID` in `PARAM.SFO`. Declaring both is allowed, but they must agree: a mismatch fails at configure time, because a transposed digit otherwise ships a package that installs under one identity and announces another. |
| `ICON` | the toolchain's `sdk/assets/ICON0.PNG` | Copied to `ICON0.PNG` in the package |
| `SFOXML` | `cmake/templates/sfo.xml` | XML template for `sfo -f` |
| `PKGFILES` | `pkg_files/` in the build directory if it exists, else `pkg_files/` in the source directory | Directory copied over the staging tree after the standard files, for extra package content (for example files under `USRDIR/`) |

The target must already have `ps3_add_self()` applied: `ps3_add_pkg` signs
the stripped `<target>.elf` that the SELF post-build chain leaves in the
target's output directory (`PS3_SELF_OUTPUT_DIRECTORY` on the target or
globally, else the current binary directory).

**Outputs**, in that same output directory:

| File | Use |
|---|---|
| `<target>.pkg` | Debug-installable package (CFW hardware, RPCS3) |
| `<target>.gnpdrm.pkg` | NPDRM-finalised copy |

The staging tree is `<build-dir>/pkg/` and is recreated on every package
build. A custom target `<target>_pkg` (part of `ALL`) drives the rule; it
reruns when the stripped ELF, the icon or the SFO template changes.

**Configure-time errors:**

- `required host tool '<tool>' not found`: one of `make_self_npdrm`, `pkg`,
  `sfo` or `package_finalize` is missing from `$PS3DEV/bin` and
  `$PS3DK/bin`. An install that only has `sfo.py`/`pkg.py` predates the
  native tools; re-extract the release zip or re-run the host-tools install.
  After upgrading an SDK in place, also delete the build directory:
  `find_program()` results are cached, so a tree configured against the old
  install keeps its `-NOTFOUND`.
- `CONTENTID is required`, `APPID '...' does not match the title-id
  embedded in CONTENTID`, `ICON not found`, `SFOXML not found`,
  `target '...' does not exist`.

All `samples/gcm` and `samples/sysutil` samples and the PSL1GHT graphical
and input ports call `ps3_add_pkg`, so any of them is a working reference.

## What ps3_add_pkg runs

The rule follows the PSL1GHT `ppu_rules` package recipe:

1. `make_self_npdrm <target>.elf <pkg>/USRDIR/EBOOT.BIN <CONTENTID>`
2. `sfo --title "<TITLE>" --appid "<APPID>" -f <SFOXML> <pkg>/PARAM.SFO`
3. copy `<ICON>` to `<pkg>/ICON0.PNG`
4. copy the `PKGFILES` directory over `<pkg>/`, if it exists
5. `pkg --contentid <CONTENTID> <pkg>/ <target>.pkg`
6. copy `<target>.pkg` to `<target>.gnpdrm.pkg`, then
   `package_finalize <target>.gnpdrm.pkg`

## Manual sequence (no CMake)

From a `cmd` shell after `%PS3DK%\setup.cmd`, with a stripped
`build\sample.elf` (see [toolchain.md](toolchain.md) for compile, link and
strip):

```cmd
mkdir build\pkg\USRDIR
copy "%PS3DK%\bin\ICON0.PNG" build\pkg\ICON0.PNG
make_self_npdrm.exe build\sample.elf build\pkg\USRDIR\EBOOT.BIN UP0001-HELPPU017_00-0000000000000000
sfo.exe --title "Hello PPU" --appid HELPPU017 -f "%PS3DK%\bin\sfo.xml" build\pkg\PARAM.SFO
pkg.exe --contentid UP0001-HELPPU017_00-0000000000000000 build\pkg\ sample.pkg
copy sample.pkg sample.gnpdrm.pkg
package_finalize.exe sample.gnpdrm.pkg
```

`sample.pkg` is unsigned (debug-installable on CFW); `sample.gnpdrm.pkg`,
after `package_finalize`, is the NPDRM-finalised version.

## pkg reference

```
pkg --contentid=<content-id> <target-directory> [out-file]
pkg -l | --list <pkg-file>
pkg -x | --extract <pkg-file>
pkg -v | --version
pkg -h | --help
```

| Option | Meaning |
|---|---|
| `-c`, `--contentid <id>` | Content id written into the package header and metadata (required for packing; at most 0x30 bytes are used) |
| `<target-directory>` | Directory to pack; a trailing separator is optional |
| `[out-file]` | Output path; default `<content-id>.pkg` in the current directory |
| `-l`, `--list <pkg>` | Print the package header and its file list |
| `-x`, `--extract <pkg>` | Extract into a directory named after the package's content id, in the current directory |
| `-d`, `--debug` | With `--extract`, print the header and each file entry (packing always prints the new header) |
| `-v`, `--version` | Print `pkg 0.6` |
| `-h`, `--help` | Print usage |

A file at `USRDIR/EBOOT.BIN` in the packed tree is stored as an NPDRM SELF
entry (with the NPDRM size rounding); everything else is stored raw.

Exit status: 0 on success; 1 when a file cannot be read or written, a
directory cannot be opened, or memory runs out (a partially written output
file is deleted); 2 for bad usage (missing `--contentid`, wrong number of
arguments, unknown option). Messages go to stderr (`Cannot open: <path>`,
`pkg: cannot open directory ...`, `PKG: file too small`,
`Unsupported Type`, ...).

The local changes to upstream `pkg.c` (our own SHA-1, Windows relative-path
and long-path fixes, failing instead of exiting 0 when a payload cannot be
opened, no fixed file-count or path-length limits) and how its output was
verified byte-for-byte are recorded in
[`tools/sfo-pkg/PROVENANCE.md`](../../tools/sfo-pkg/PROVENANCE.md).

## make_self_npdrm and package_finalize

Both are PSL1GHT host tools, shipped unchanged in behaviour:

```
make_self_npdrm <input.elf> <output.self> <content-id>
package_finalize <file.pkg>
```

`make_self_npdrm` writes an NPDRM SELF; the output name matters to NPDRM, so
write it directly as `USRDIR/EBOOT.BIN` rather than renaming it afterwards.
`package_finalize` rewrites the given `.pkg` in place, which is why the
CMake rule finalises a copy.

## See also

- [sfo.md](sfo.md): `PARAM.SFO` editing and the `sfo` generator
- [docs/sdk/param-sfo.md](../sdk/param-sfo.md): key and flag reference
- [toolchain.md](toolchain.md): compile, link, strip and SELF signing
