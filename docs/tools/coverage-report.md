# coverage-report

`coverage-report` measures how much of the PS3 system-library export surface
an installed SDK tree provides. It reads a tracked export list (one YAML file
per system stub library, with every exported name and its NID), scans every
`.a` archive under an install tree for the global symbols it defines, and
writes a Markdown matrix: per library, how many tracked exports the install
tree defines, and which names are still missing.

The published matrix is [docs/coverage.md](../coverage.md).

## Install and locate

`coverage-report` is a binary of the Rust workspace in `tools/` (crate
`tools/coverage-report`).

| Host | Location |
|---|---|
| Linux (from source) | `$PS3DEV/bin/coverage-report`, mirrored to `$PS3DK/bin/coverage-report` by `scripts/install-host-tools.sh` (run by `scripts/build-sdk.sh` unless `--no-host-tools`) |
| Windows release zip | `%PS3DK%\bin\coverage-report.exe` |
| Workspace build | `cd tools && cargo build --release -p coverage-report` puts it in `tools/target/release/` (Rust 1.88 or newer) |

`coverage-report --version` prints the tools version (`tools/VERSION`).

## Inputs

- **Export list** (`--nid-db`): a directory of library YAML files in the
  nidgen database schema (`tools/nidgen/nids/schema.yaml`). The tracked list
  is `tools/nidgen/nids/extracted/`, one file per stub library
  (`libaudio_stub.yaml`, ...). Only files ending in `.yaml` directly in that
  directory are read; subdirectories are not searched. The library is named
  in the report by the file stem (`libaudio_stub`).
- **Install tree** (`--install`): any directory. It is walked recursively
  (symbolic links are followed) and every file ending in `.a` is opened as an
  archive. From each big-endian ELF64 member the tool collects global,
  defined symbols, skipping the dot-prefixed function-entry aliases
  (`.cellPadInit`), so a function counts by its descriptor name. Non-ELF
  members, little-endian members and archive index members are skipped.
- **Alias map** (`--alias-map`, optional): a YAML file mapping a tracked
  export name to one or more names that satisfy it in the install tree, for
  PSL1GHT-style renames such as `sys_lwmutex_lock` -> `sysLwMutexLock` or
  `cellMsgDialogOpen` -> `msgDialogOpen`:

  ```yaml
  aliases:
    cellMsgDialogOpen:
      - msgDialogOpen
  ```

  The tracked map is `tools/nidgen/nids/aliases.yaml`.

An export counts as covered when the install tree defines it by its exact
name, or when the alias map lists a name for it that the install tree
defines.

## Usage

Run from the repository root (the `--output` and `--alias-map` defaults are
relative paths):

```bash
# Regenerate docs/coverage.md against a staged or installed tree
coverage-report --nid-db tools/nidgen/nids/extracted --install stage/ps3dev

# Against an installed SDK, writing somewhere else, listing every missing name
coverage-report --nid-db tools/nidgen/nids/extracted --install "$PS3DK" \
    --output /tmp/coverage.md --list-missing 0

# Refresh the alias map from the current install tree
coverage-report --nid-db tools/nidgen/nids/extracted --install stage/ps3dev \
    --suggest-aliases tools/nidgen/nids/aliases.yaml
```

## Options

| Option | Default | Meaning |
|---|---|---|
| `--nid-db <DIR>` | required | Directory of library YAML files (the export list). |
| `--install <DIR>` | required | Install tree root to scan for `.a` archives, typically `stage/ps3dev` or `$PS3DK`. |
| `--output <PATH>` | `docs/coverage.md` | Markdown report path. Parent directories are created. |
| `--list-missing <N>` | `500` | How many missing names to list per library; `0` lists all. Longer lists end with `... (<n> more)`. |
| `--alias-map <PATH>` | `tools/nidgen/nids/aliases.yaml` | Alias YAML. If the file does not exist the run continues without aliases and prints `note: alias map <path> not found — running raw-name coverage`. |
| `--suggest-aliases <PATH>` | off | Instead of writing the report, write a candidate alias map to `<PATH>` and exit. `--output` and `--alias-map` are ignored. |
| `--version`, `--help` | | Print the version or usage. |

### --suggest-aliases

For every tracked export that the install tree does not define by exact name,
the tool builds rename candidates with four rules and keeps only candidates
the install tree actually defines:

| Rule | From | To | Example |
|---|---|---|---|
| R1 | `_sys_<snake>` | `sys<CamelSnake>` | `_sys_malloc` -> `sysMalloc` |
| R2 | `sys_<snake>` | `sys<CamelSnake>` | `sys_lwmutex_lock` -> `sysLwMutexLock` |
| R3 | `cell<CamelRest>` | `<camelRest>` | `cellMsgDialogOpen` -> `msgDialogOpen` |
| R4 | `cell_<snake>` | `<CamelSnake>` | |

The file is overwritten with the generated map, so hand-added aliases are
lost unless you merge them back; review the diff before committing. It prints
`wrote <path> (<n> candidate aliases)` on stderr.

## Output

The report is Markdown with three parts:

1. **Summary**: libraries tracked, total tracked exports, how many are covered
   (percentage, split into exact-name and alias matches), the number of
   distinct symbols in the install tree, the number of alias entries loaded
   (or *not loaded*), and a tally of the `impl_status` annotations in the
   YAML files (unknown, stub, impl, verified).
2. **Per-library coverage**: one table row per library with its export count,
   exact matches, alias-only matches, total covered and percentage.
3. **Missing symbols by library**: for each library with gaps, a code block of
   `0x<nid>  <name>` lines, capped by `--list-missing`. Fully covered
   libraries are omitted.

On success the tool prints `wrote <path>` on stderr.

The `Tool version` and `Command` lines at the top of
[docs/coverage.md](../coverage.md) are not written by the tool; they are
added by hand when the published matrix is refreshed, to record which build
and install tree produced it.

`tools/docs/coverage.md` is an early output kept from the tool's first runs;
it predates the alias map and most of the SDK and is not maintained. The
current matrix is `docs/coverage.md`.

## Errors and exit status

The tool exits 0 on success and 1 on any error, printing the error chain.

- `reading <dir>`: the `--nid-db` directory cannot be read.
- `parsing <file>`: a library YAML (or the alias map) is not valid for the
  schema.
- `writing <path>` / `creating <dir>`: the output cannot be written.
- `warn: skipping <archive>: <reason>`: an `.a` file that cannot be read as an
  archive is skipped and the run continues; its symbols are not counted.

Coverage gaps are not errors: a report with missing names still exits 0.

## Build integration

`coverage-report` is not run by the SDK build, the sample builds or CI. The
coverage matrix is refreshed by hand after a release or a significant SDK
change, and the alias map is refreshed with `--suggest-aliases` after a
significant PSL1GHT or install-tree change. It is a measurement tool: it
reads archives and writes Markdown, and never changes the install tree.

Related: [nidgen](nidgen.md) owns the YAML schema and the export list, and
generates the stub archives whose symbols this tool counts.
