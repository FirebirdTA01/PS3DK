# sfo and sfo-gui: PARAM.SFO tools

Every installable PS3 application, save, patch and trophy set carries a
`PARAM.SFO`: the title, the title id, the category, the `ATTRIBUTE`
capability bits (remote play, Move support, ...), the resolution and sound
masks, and so on. The SDK ships one SFO engine (the `sfo` crate in
`tools/sfo`) with two front-ends:

| Tool | Use it for |
|---|---|
| `sfo` | Everything from the shell or a script: inspect, validate, edit, add, remove and rename params, flip flag bits, create from a template, and the one-shot XML generator that package builds use |
| `sfo-gui` | The same operations in a desktop window: every registry-known key and flag bit as a labelled widget |

Both are generated from one key and flag registry,
[`tools/sfo/registry/param-sfo.yml`](../../tools/sfo/registry/param-sfo.yml),
which also generates the reference document
[docs/sdk/param-sfo.md](../sdk/param-sfo.md). The CLI, the GUI and that
reference therefore cannot drift apart.

## Design rules

- **Preservation first.** Keys and flag bits the registry does not know are
  kept byte-exact. Edits keep every entry's reserved slot size
  (`param_max`), so the file layout does not reflow unless you ask for it
  with `--grow`.
- **No in-place writes from the CLI.** Every editing subcommand takes a
  required `--out`; the input file is never modified. (`--out` may name the
  input file if you do want to overwrite it.)
- **Registry-aware, not registry-bound.** Flag names, key formats and
  default slot sizes come from the registry, but unknown keys can still be
  added (with an explicit `--type`), edited and removed.

## Install and locate

| Host | Location |
|---|---|
| Windows release | `%PS3DK%\bin\sfo.exe`, `%PS3DK%\bin\sfo-gui.exe` (on `PATH` after `setup.cmd`) |
| Linux, from source | `$PS3DEV/bin/sfo` and `$PS3DEV/bin/sfo-gui`, mirrored into `$PS3DK/bin/` |

`scripts/build-sdk.sh` builds and installs both through
`scripts/install-host-tools.sh` (skip with `--no-host-tools`). To build them
on their own:

```bash
cd tools
cargo build --release -p sfo
# tools/target/release/sfo and tools/target/release/sfo-gui
```

`sfo --version` (or `-v`) prints `sfo <version>`; `sfo-gui --version` prints
`sfo-gui <version>`.

## CLI reference

```
sfo <SUBCOMMAND> ...                      subcommand form
sfo [-f] [--title T] [--appid ID] in.xml out.sfo    legacy generator
sfo -t in.sfo out.xml                     legacy SFO -> XML
sfo -l in.sfo [-d [-p]]                   legacy listing
sfo -v | --version
sfo --help | sfo <SUBCOMMAND> --help
```

Integers given on the command line (`set`, `add` on an integer key) accept
decimal or `0x`-prefixed hexadecimal. Array and raw values are hex byte
strings (`"01 02 0a ff"`; whitespace is ignored, the digit count must be
even).

### inspect

```
sfo inspect <INPUT> [--json] [--schema <SCHEMA>]
```

Prints every entry. Without `--json` the output is a one-line dictionary,
for example `{'APP_VER': '01.00', 'ATTRIBUTE': 0, ..., 'TITLE': 'My Game'}`
(array and raw values print as `0`). With `--json` it prints a JSON array,
one object per entry:

| Field | Meaning |
|---|---|
| `key` | Key name |
| `format` | `utf8`, `integer`, `array` (`0x0004`), `utf8_raw` or `raw` |
| `param_fmt` | Raw format code from the file (`0x0004`, `0x0204`, `0x0404`, ...) |
| `value_len` | Bytes used |
| `max_len` | Reserved slot size |
| `value` | String, integer, or hex string for array/raw values |
| `registry` | `null` for a key the registry does not know; otherwise `schema`, `known`, `confidence` (`confirmed`, `observed`, `speculative`, `reserved`, `gap`), `source`, and `decoded_flags` (names of the registry bits set in an integer value) |

### validate

```
sfo validate <INPUT> [--schema <SCHEMA>]
```

Parses the container, then checks every registry-known key's stored format
against the registry (for example `TITLE` must be UTF-8, `ATTRIBUTE` an
integer). Prints `PARAM.SFO OK` on success. Keys the registry does not know,
and keys whose registry format is unknown, are skipped. In the save-data
schema, `PARAMS` and `PARAMS2` stored as UTF-8 are accepted.

### set

```
sfo set <INPUT> KEY=VALUE --out <OUT> [--grow]
```

Changes the value of an existing key. The key keeps its format: an integer
key takes an integer, a string key takes the text after `=`, an array or raw
key takes hex bytes. The key must already exist (use `add` otherwise).

If the new value does not fit the key's reserved slot, the write is
refused with `value for KEY needs N bytes but preserved max is M`. With
`--grow`, the named key's slot is enlarged to the canonical size for that
value, only that entry moves, and `KEY max_len OLD -> NEW` is printed on
stderr.

### add

```
sfo add <INPUT> <KEY> --value <VALUE> --out <OUT>
        [--type <TYPE>] [--max-len <N>] [--grow]
```

Adds a key the file does not carry (retail SFOs often omit `ATTRIBUTE`, for
example). The key must not already exist.

- `--type` picks the stored format: `utf8` (or `string`), `integer` (or
  `int32`), `array`, or `raw:<hex format code>` (for example `raw:0x0204`;
  the value is then hex bytes). For a registry-known key it may be omitted
  and the registry format is used; a key the registry does not know, or
  knows with an unknown format, needs `--type`.
- `--max-len` sets the reserved slot size. Default: the registry's
  `max_len` for the key, otherwise the canonical size of the value (raw
  values round up to a multiple of 4).
- `--grow` enlarges the slot if the value is longer than `--max-len`, as for
  `set`.

The registry lookup for `add` uses the game schema.

### flags

```
sfo flags <INPUT> <KEY> (--enable <FLAG> | --disable <FLAG>) --out <OUT>
          [--schema <SCHEMA>]
```

Sets or clears one named bit in an integer bitmask key (`ATTRIBUTE`,
`RESOLUTION`, `SOUND_FORMAT`, `REGION_DENY`, ...). Both `--enable` and
`--disable` may be given in one call (enable is applied first); at least one
is required. The flag name, or one of its registry aliases, must exist for
that key in the current schema; otherwise the error is
`SFO registry key 'KEY' has no flag 'NAME'` (the tool prints the names in backticks). The key must already be present
in the file: add it first with `sfo add <in> ATTRIBUTE --value 0`.

### remove and rename

```
sfo remove <INPUT> <KEY> --out <OUT>
sfo rename <INPUT> <FROM> <TO> --out <OUT>
```

`remove` deletes a key outright (error `SFO entry 'KEY' not found` if absent).
`rename` changes a key's name and keeps its value and slot; it refuses a
`<TO>` that already exists.

### create

```
sfo create --template game --out <OUT> [--title <TITLE>] [--appid <ID>]
```

Writes a new `PARAM.SFO` from a built-in template. `game` is the only
template: a bootable HDD game (`CATEGORY` `HG`, `BOOTABLE` 1,
`APP_VER`/`VERSION` `01.00`, `PS3_SYSTEM_VER` `01.8000`, `ATTRIBUTE` 0,
`PARENTAL_LEVEL` 0, `RESOLUTION` 63, `SOUND_FORMAT` 279, a `LICENSE` string,
`TITLE` `PS3DK Sample`, `TITLE_ID` `PS3DK0001`). `--title` replaces `TITLE`
and `--appid` replaces `TITLE_ID`. The template is
[`cmake/templates/sfo.xml`](../../cmake/templates/sfo.xml), compiled into
the tool. Any other template name fails with
`unsupported SFO template`.

### docs

```
sfo docs [--check]
```

Prints the Markdown reference generated from the bundled registry (the
content of [docs/sdk/param-sfo.md](../sdk/param-sfo.md)): container format,
sources, category values, and every key with its format, slot size, flag
bits and behaviour. `--check` instead compares `docs/sdk/param-sfo.md`
(relative to the current directory, so run it from the repository root)
with the generated text and fails if they differ; the repository's tests use
it to keep the document current.

### Schemas (`--schema`)

`inspect`, `validate` and `flags` pick the registry schema from the file's
`CATEGORY`, and `--schema` overrides it:

| Detected from | Schema | Flag table used for `ATTRIBUTE` |
|---|---|---|
| `CATEGORY` `SD` or `MS` | `savedata` | save-data keys |
| `CATEGORY` `TR`, `VR`, `DP` or `XR` | `subfolder` | game keys, subfolder `ATTRIBUTE` table |
| `CATEGORY` `GD` with `APP_VER` or `TARGET_APP_VER` | `patch` | game keys, patch `ATTRIBUTE` table |
| anything else | `game` (alias `bootable`) | game keys |
| `--schema trophy` only | `trophy` | trophy keys |

Any other `--schema` value fails with `unsupported SFO schema`.

### Legacy one-shot invocation

Releases up to v0.12.0 shipped `sfo` as a small C generator adopted from
upstream PSL1GHT. The Rust CLI accepts that tool's command line with
byte-identical file output, so existing Makefiles and `ps3_add_pkg` keep
working:

| Form | Effect |
|---|---|
| `sfo [--title T] [--appid ID] -f in.xml out.sfo` | Build `out.sfo` from an XML template; `--title` replaces `TITLE`, `--appid` replaces `TITLE_ID` (`-f`/`--fromxml`) |
| `sfo -t in.sfo out.xml` | Write the SFO's string and integer entries as XML (`-t`/`--toxml`), and print the dictionary listing unless `-d` is given |
| `sfo -l in.sfo` | Print the dictionary listing (`-l`/`--list`) |
| `sfo -l in.sfo -d` | Print only `SFO entries: N` (`-d`/`--debug`) |
| `sfo -l in.sfo -d -p` | Print one `[X] Key: 'NAME'` line per entry (`-p`/`--pretty`) |

The XML format is a `<sfo>` root holding
`<value name="KEY" type="string|integer">VALUE</value>` elements; any other
`type` is an error. The generator writes canonical slot sizes. `-f` and `-t`
together, or the wrong number of file arguments, fail with
`invalid arguments; use --help for usage`.

```cmd
sfo.exe --title "My Game" --appid MYGAME001 -f "%PS3DK%\bin\sfo.xml" PARAM.SFO
```

`sfo.xml` (the PSL1GHT-style default template) is installed beside the tool
in `%PS3DK%\bin` / `$PS3DEV/bin`.

## Examples

```cmd
:: Show every key (add --json for machine-readable output)
sfo.exe inspect PARAM.SFO

:: Check types against the registry
sfo.exe validate PARAM.SFO

:: Edit one value (rejects a value that will not fit; add --grow to resize)
sfo.exe set PARAM.SFO "TITLE=My Game" --out PARAM.SFO.new

:: Add a param a file does not carry
sfo.exe add PARAM.SFO ATTRIBUTE --value 0 --out PARAM.SFO.new

:: ...then flip capability bits on it by registry name
sfo.exe flags PARAM.SFO.new ATTRIBUTE --enable psvita_remote_play --out PARAM.SFO.new2
sfo.exe flags PARAM.SFO ATTRIBUTE --disable psp_remote_play_v1 --out PARAM.SFO.new

:: Same for the video-mode mask
sfo.exe flags PARAM.SFO RESOLUTION --enable hd_720 --out PARAM.SFO.new

:: Remove or rename a param
sfo.exe remove PARAM.SFO ATTRIBUTE --out PARAM.SFO.new
sfo.exe rename PARAM.SFO TITLE_01 TITLE_02 --out PARAM.SFO.new

:: Start a new SFO from the game template
sfo.exe create --template game --title "My Game" --appid MYGAME001 --out PARAM.SFO

:: Key and flag reference, generated from the registry
sfo.exe docs
```

The flag names (`psvita_remote_play`, `hd_720`, ...) come from the
registry; `sfo docs` or [docs/sdk/param-sfo.md](../sdk/param-sfo.md) lists
every key, every known bit, and what it does.

## Exit status and errors

`sfo` and `sfo-gui` exit 0 on success and 2 on any error, with the message
on stderr prefixed `sfo:` (or `sfo-gui:`). Argument errors from the parser
(unknown option, missing `--out`) also exit 2. Common messages:

Key and flag names appear in backticks in the real messages; they are
shown in single quotes here.

| Message | Cause |
|---|---|
| `SFO: bad magic ...`, `SFO: unexpected version ...`, `SFO: file too small`, `SFO: truncated entry N`, `SFO: duplicate key K`, `... extends past end of file`, `... is not NUL-terminated` | The input is not a well-formed PARAM.SFO |
| `SFO entry 'K' not found` | `set`, `flags`, `remove` or `rename` named a key the file does not have |
| `SFO entry 'K' already exists` | `add`, or `rename` to an existing name |
| `value for K needs N bytes but preserved max is M` | Value longer than the slot; use `--grow` |
| `SFO entry 'K' needs --type because its registry format is unknown` | `add` of an unknown key without `--type` |
| `SFO registry has no key 'K'` / `... is not an integer bitfield` / `... has no flag 'F'` | `flags` on a key or bit the schema does not define |
| `SFO entry 'K' has format X, expected Y` | `validate` found a registry-known key stored in the wrong format |
| `flags requires --enable or --disable` | Neither flag option given |

## sfo-gui

`sfo-gui` is a desktop editor over the same engine.

- **Open...** (Ctrl+O) loads a file through the native file dialog;
  **New** (Ctrl+N) creates a game PARAM.SFO from the same template as
  `sfo create`. `sfo-gui --open <path>` opens a file at start-up.
- **Save** (Ctrl+S) writes the opened file in place; **Save As...**
  (Ctrl+Shift+S) writes a new file. Saving uses the preserving writer, so a
  save with no edits is byte-identical to the input. Opening another file or
  closing with unsaved edits asks first.
- Every registry-known key renders as a typed widget with a
  bytes-used/bytes-reserved readout. Bitmask keys (`ATTRIBUTE`,
  `RESOLUTION`, `SOUND_FORMAT`, `REGION_DENY`) render as grouped, labelled
  checkboxes, with remote-play bits under their own group, badges and
  tooltips from the registry, and reserved bits hidden unless **Show
  reserved bits** is on. The `ATTRIBUTE` bits shown follow the file's
  category, as in the CLI.
- Keys the file does not carry show as **Not set** rows with an **Add**
  button, and are listed on the **Add a key** page, so giving a retail SFO an `ATTRIBUTE` (to enable Vita
  remote play, say) is: open, Add, tick the box, Save.
- Keys the registry does not know are shown and preserved, never dropped,
  and an edited value can be reverted.
- A value longer than its slot is refused unless **Grow value slots** is on in
  the settings, matching `--grow` on the CLI.

`sfo-gui` also has hidden command-line options (`--save`, `--save-as`,
`--gui-set`, `--gui-add`, ...) that drive the editor model without a window;
they exist for the tool's own tests and are not a supported interface. Use
`sfo` for scripting.

## Build integration

`ps3_add_pkg()` in `cmake/ps3-self.cmake` runs the legacy generator form to
produce each package's `PARAM.SFO`:

```
sfo --title "<TITLE>" --appid "<APPID>" -f <SFOXML> <pkg>/PARAM.SFO
```

`SFOXML` defaults to `cmake/templates/sfo.xml`. See [pkg.md](pkg.md) for the
whole package pipeline. PSL1GHT Makefiles that call
`sfo ... -f $(PS3DEV)/bin/sfo.xml` work unchanged.

## History and test fixtures

Releases up to v0.12.0 shipped `sfo` as the C generator from upstream
PSL1GHT, with separate editors alongside as `sfo-editor` / `sfo-editor-gui`.
The Rust CLI took over the `sfo` name once it matched that tool's command
line and output byte for byte; the C tool is no longer built.

Its source stays in [`tools/sfo-pkg/sfo.c`](../../tools/sfo-pkg/sfo.c) for
one purpose: the editors' correctness is anchored to checked-in golden
fixtures (`tools/sfo/tests/fixtures/`) generated from that frozen C tool. If
those fixtures ever need regenerating, build `sfo.c` from the repository;
never regenerate them from an installed `sfo`, which would test the tool
against itself.

## See also

- [docs/sdk/param-sfo.md](../sdk/param-sfo.md): generated key, category and
  flag reference
- [pkg.md](pkg.md): building installable `.pkg` files
