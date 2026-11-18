# Overlay tools: cellOvisMkLdscript and cellOvisConfigAuto

SPU local store is 256 KB. Code overlays let an SPU program hold more code
than fits by giving several *sections* the same local-store addresses and
loading whichever one is needed at run time. The runtime half is `libovis`
(`<cell/ovis.h>`, linked as `-lovis` on both SPU and PPU); these two host
tools write the link-time half:

| Tool | Layout comes from |
|---|---|
| `cellOvisMkLdscript` | An XML `<ovis_config>` file that lists regions, sections and objects. |
| `cellOvisConfigAuto` | The object files themselves: one region, one section per object, every global function wrapped. |

Source: `tools/ovis` (Rust workspace, crate `ovis`).

## What the tools generate

An overlaid program has one or more overlay *regions*; each region holds
sections that share its local-store range, and each section is built from the
`.text` and `.rodata` of some objects. The tools write:

- **A linker script** (inserted with `INSERT BEFORE .text` and `INSERT AFTER .data`)
  that places the sections of region N at one VMA after the end of every earlier
  region, gives each section its own load address from `0x40000` (the end of
  local store) upward so each is a separate loadable segment, and lays down the
  overlay table `_ovly_table` after `.data`: one 16-byte entry
  `{vma, size, lma, 0}` per section, labelled `__ovly_info_<section>`, followed
  by the section count `_novlys`. Section alignment is 128 bytes for XML
  layouts and 16 bytes in automatic mode.
- **Call wrappers** (SPU assembly): for each wrapped function `f`,
  `__wrap_f` maps the section that holds `f` through
  `_cellOvisUpdateAndMapSection`, then branches to `__real_f` with the
  original arguments and return address.
- **Link options** (one line): `-Wl,--no-overlays` (the SPU linker would
  otherwise build its own overlay manager and tables and clash with libovis),
  then one `-Wl,--wrap=<f>` per wrapped function.

On the PPU, `cellOvisInitializeOverlayTable` builds the overlay table and
`cellOvisFixSpuSegments` drops the overlay segments from the image before it
is loaded.

## Install and locate

`scripts/build-sdk.sh` installs `cellOvisMkLdscript` and `cellOvisConfigAuto`
into `$PS3DEV/bin` and `$PS3DK/bin` on Linux; the Windows SDK zip ships them as
`.exe` in `%PS3DK%\bin`. Build them alone with
`cargo build --release --manifest-path tools/Cargo.toml -p ovis`.

## cellOvisMkLdscript

```
cellOvisMkLdscript [--ldscript=FILE] [--ldflags=FILE] [--wrapper=FILE] [--spurs] config.xml
```

| Option | Meaning |
|---|---|
| `--ldscript=FILE` | Write the linker script to FILE. Without it the script goes to standard output. |
| `--ldflags=FILE` | Write the link options for the functions the XML lists. |
| `--wrapper=FILE` | Write the wrapper assembly for the functions the XML lists. |
| `--spurs` | Accepted for old makefiles; changes nothing and prints a note. Link a SPURS task with `-mspurs-task` and this script. |
| `-v`, `--version` | Print `cellOvisMkLdscript <version>`. |
| `-h`, `--help` | Print usage (exit 2). |

Exactly one configuration file is taken; a second positional argument or any
other `--` option is an error.

### Configuration file

```xml
<?xml version="1.0"?>
<ovis_config>
  <overlay>                                   <!-- one region -->
    <sections>
      <section name="sec_add">
        <object name="op_add.c.o"/>
      </section>
    </sections>
    <sections>
      <section name="sec_mul">
        <object name="op_mul.c.o">
          <function name="mul"/>              <!-- optional: wrap calls to mul -->
        </object>
      </section>
    </sections>
  </overlay>
  <overlay>                                   <!-- a second region -->
    <sections>
      <section name="sec_fib"><object name="op_fib.c.o"/></section>
    </sections>
  </overlay>
</ovis_config>
```

Elements nest exactly as
`ovis_config > overlay > sections > section > object > function`.
`section`, `object` and `function` need a non-empty `name` attribute; section
and function names must be C identifiers. Comments, the `<?xml?>` declaration
and text are ignored. The parser refuses an element in the wrong place
(`unexpected <x> inside <y>`), mismatched or unclosed tags, a configuration
with no sections, a section listed twice, a section with no object, and a
name that is not a valid symbol.

Object names are matched by the linker against the input file names, so they
must match how the objects are named on the link line (with CMake, see below).

## cellOvisConfigAuto

```
cellOvisConfigAuto --ldscript=FILE --ldflags=FILE --wrapper=FILE [--nm=PROGRAM] object.o...
```

| Option | Meaning |
|---|---|
| `--ldscript=FILE` | Required. Linker script output. |
| `--ldflags=FILE` | Required. Link options output. |
| `--wrapper=FILE` | Required. Wrapper assembly output. |
| `--nm=PROGRAM` | Accepted for old makefiles and ignored; the objects are read directly. |
| `-v`, `--version` | Print `cellOvisConfigAuto <version>`. |
| `-h`, `--help` | Print usage (exit 2). |

At least one object is required; duplicates are dropped. Each object becomes
one section in a single region, named after its path with `/` and `\` turned
into `__` and other non-symbol characters into `_` (`objs/sort.spu.o` becomes
`objs__sort_spu_o`). Every global function defined in its `.text` or
`.text.*` sections is wrapped. An object with no global function draws a
warning (`defines no global function; nothing will map it`). A file that is
not an SPU object is an error.

Link the program with `-T<ldscript>`, the options in `<ldflags>` and the
assembled `<wrapper>`, and call `cellOvisInitializeAutoMapping` before the
first overlaid call.

## Exit status

0 on success; 2 on a usage error; 1 when a file cannot be read or written or
the configuration or objects are rejected (message `<tool>: <file>: <reason>`).

## Build integration

`ps3_add_spu_image` in `cmake/ps3-self.cmake` drives both tools:

```cmake
ps3_add_spu_image(app NAME ovis_manual
    SOURCES spu/manual.c ${ops}
    OVIS_CONFIG spu/manual.xml)          # cellOvisMkLdscript

ps3_add_spu_image(app NAME ovis_auto
    SOURCES spu/auto.c ${ops}
    OVIS_AUTO ${ops})                    # cellOvisConfigAuto; also list them in SOURCES
```

With `OVIS_CONFIG`, objects are named `<source file>.o`, for example
`<object name="sort.c.o"/>`. Either option adds the generated script, the
assembled wrappers, `-Wl,--no-overlays` and `-lovis` to the SPU link.
Configure fails if the needed tool is not in `$PS3DEV/bin` or `$PS3DK/bin`.
Link `-lovis` on the PPU target too. The rest of `ps3_add_spu_image` is
described in [spu-elf-to-ppu-obj.md](spu-elf-to-ppu-obj.md).

Samples: `samples/spu/hello-ovis` (both modes: an XML layout with two regions
mapped by hand, and an automatic layout mapped on each call) and
`samples/spu/hello-ppu-ovis` (the PPU-side calls).
