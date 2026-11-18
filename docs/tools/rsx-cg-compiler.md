# rsx-cg-compiler

`rsx-cg-compiler` compiles Cg vertex and fragment shaders to RSX (NV40)
microcode and wraps the result in a binary program container that the GCM
and PSGL runtimes load. It is the recommended shader compiler for this SDK.
PSL1GHT's `cgcomp` is still bundled for older Makefiles (see
[cgcomp compatibility](#cgcomp-compatibility)).

This page covers the command line, the language surface, the output
formats, the inspection switches, the comparison and regression tooling in
the repository, and build integration.

- [Overview](#overview)
- [Install and locate](#install-and-locate)
- [Quick start](#quick-start)
- [CLI reference](#cli-reference)
- [Profiles and language features](#profiles-and-language-features)
- [Outputs](#outputs)
- [PSGL fixed-function shader library](#psgl-fixed-function-shader-library)
- [Inspection and dump tools](#inspection-and-dump-tools)
- [Comparison and regression tooling](#comparison-and-regression-tooling)
- [Build integration](#build-integration)
- [Diagnostics](#diagnostics)
- [Known limits](#known-limits)
- [Troubleshooting](#troubleshooting)
- [For contributors](#for-contributors)

## Overview

The compiler is a self-contained C++17 program under
`tools/rsx-cg-compiler/`:

```
.cg / .vcg / .fcg source
    |
    v
front end      src/cg_frontend/   preprocessor, lexer, parser, semantic
                                  analysis, IR builder, IR passes
    |  IR module (hardware independent)
    v
NV40 back end  src/nv40/          discard guards, if-conversion, general
                                  lowering, register allocation, ucode emit
    |  NV40 instruction words
    v
containers     src/cg_container_{vp,fp}.cpp
    |
    v
.vpo / .fpo (binary program container) or compact CGB container
```

The front end comes from the MIT-licensed open-source Vita Cg compiler
project and lives in this tree, so the build needs no extra checkout. The
NV40 back end and the container writers are specific to this project. There
is one lowering path (the "general lowering"); the older shape-matching back
end has been removed.

The goal is output compatible with the established compiler for the same
source: same accepted language, same container layout and parameter tables.
Differences, including optimisation differences, are tracked; byte identity
is not the release bar. Language beyond that surface is available only
through named, opt-in [extensions](#extensions).

The same CMake project also builds a second executable, the **ELF shader
archive packer**, which packs `.vpo`/`.fpo` containers into one ELF32
archive (see [Shader archive packer](#shader-archive-packer)).

## Install and locate

Every SDK release installs `rsx-cg-compiler` (plus the archive packer) into
`$PS3DEV/bin` and `$PS3DK/bin`. On Windows the release zip ships
`rsx-cg-compiler.exe`.

```
rsx-cg-compiler --version      # prints "rsx-cg-compiler <sdk version>"
```

The version is the repository-wide SDK version (`scripts/version.sh`), so the
compiler always reports the same version as the SDK it came with.

To build it from source (Linux, WSL, or any host with CMake 3.16+, a C++17
compiler and Python 3):

```
cmake -S tools/rsx-cg-compiler -B tools/rsx-cg-compiler/build
cmake --build tools/rsx-cg-compiler/build --target rsx-cg-compiler
```

The configure step runs `scripts/version.sh` through `bash` (bash must be on
`PATH`, also on Windows) and embeds the built-in Cg header with Python. The
default build type is `Release`. `scripts/install-host-tools.sh` (run by
`scripts/build-sdk.sh`) builds both executables and installs them into
`$PS3DEV/bin` and `$PS3DK/bin`.

The CMake helper and the libdbgfont Makefile look for the compiler in this
order: `$PS3DEV/bin`, `$PS3DK/bin`, then `tools/rsx-cg-compiler/build`.

## Quick start

A fragment shader, `tint.fcg`:

```c
void main(float4 uv : TEXCOORD0,
          uniform sampler2D tex,
          uniform float4 tint,
          out float4 color : COLOR)
{
    color = tex2D(tex, uv.xy) * tint;
}
```

Compile it to a container:

```
rsx-cg-compiler -p sce_fp_rsx --emit-container tint.fpo tint.fcg
```

and a vertex shader:

```
rsx-cg-compiler -p sce_vp_rsx --emit-container pos.vpo pos.vcg
```

Embed the `.fpo`/`.vpo` in your program (CMake: `ps3_add_cg_shader_rsxcgc`,
Make: `bin2s`) and load it with the `cellGcmCg*` API (`cellGcmCgInitProgram`,
`cellGcmCgGetUCode`, `cellGcmCgGetNamedParameter`, ...) or with PSGL
`cgCreateProgram(..., CG_BINARY, ...)`.

Without `--emit-container` the compiler writes no file; it prints the
microcode words to stdout instead, which is useful for a quick look (see
[Microcode dump](#microcode-dump)).

## CLI reference

```
rsx-cg-compiler [options] <shader.cg>
rsx-cg-compiler --psgl-ffp-library <state.txt> -o <shaders.bin>
```

Exactly one input file is accepted. Options and the input may appear in any
order (except `--psgl-ffp-library`, which must be first). Every option is a
separate argument: `-I dir`, not `-Idir`; `-p sce_fp_rsx`, not
`-psce_fp_rsx`.

### Compilation

| Option | Default | Effect |
|---|---|---|
| `-p`, `--profile <name>` | `sce_vp_rsx` | Target stage. `sce_vp_rsx` compiles a vertex program, `sce_fp_rsx` a fragment program. Any other name is refused (`unknown profile`, exit 1). The default is the **vertex** profile, so always pass `-p` for fragment shaders. |
| `-e`, `--entry <name>` | `main` | Name of the entry function. An entry that does not exist is a semantic error. |
| `-I <dir>` | none | Add a directory to the `#include` search path. Repeatable; searched in the order given. |
| `--no-stdlib` | off | Do not prepend the embedded Cg standard-library header (the built-in declarations of the Cg intrinsics). Only useful for front-end testing; ordinary shaders need it. |
| `--extension=<name>` | none | Enable one named language extension. Repeatable, one name per flag, exact case-sensitive spelling; there is no "all". An unknown name is refused before any file is read. `--extension` without `=<name>` is refused. See [Extensions](#extensions). |
| `--list-extensions` | | Print the supported extension names with a one-line summary each (tab separated) and exit 0. |

### Output

| Option | Default | Effect |
|---|---|---|
| `--emit-container <path>` | none | Write the binary program container (`.vpo`/`.fpo`) to `<path>`. |
| `--emit-cgb-container <path>` | none | Write the compact CGB container instead (see [Outputs](#outputs)). Mutually exclusive with `--emit-container`; giving both is an error. |

With neither option, nothing is written to disk and the microcode words are
printed to stdout.

### Optimisation

| Option | Default | Effect |
|---|---|---|
| `-O0`, `-O1`, `-O2`, `-O3` (also `--O0` ... `--O3`) | `-O1` | Optimisation level, named to match the established compiler's levels. |
| `-O4` / `--O4` | | `-O3` plus optimisations beyond what the established compiler applies; reserved for this compiler's own improvements. |
| `--fastmath` | on | Fast-math. |
| `--nofastmath` | | Disable fast-math. |

**Current behaviour:** the levels and fast-math switches are accepted and
recorded, but no pass in this compiler is gated on them yet, so every level
and both fast-math settings produce the same output today. The cleanup the
compiler always does (algebraic simplification, common-subexpression
elimination, dead-code elimination, constant folding and constant sharing,
if-conversion, unrolling of loops with constant bounds) runs at every level.
Pass the flags your project wants; they will start to matter as passes gate
on them, and the default (`-O1`, fast-math on) matches the established
compiler's no-flag default so default builds stay comparable.

### Inspection

| Option | Effect |
|---|---|
| `--dump-preprocess` | Print the preprocessed (macro-expanded) source to stdout and stop, like a C compiler's `-E`. Includes the built-in header unless `--no-stdlib` is given. |
| `--dump-ast` | Print the parsed syntax tree to stdout, then continue compiling. |
| `--dump-ir` | Print the IR module, as it is handed to the NV40 back end, to stdout, then continue. |
| `--stats` | Print one line of program statistics to stderr (see [Program statistics](#program-statistics)). Output files are unchanged. |

### Other

| Option | Effect |
|---|---|
| `-h`, `--help` | Print usage to stderr and exit 0. |
| `-V`, `--version` | Print `rsx-cg-compiler <version>` and exit 0. |
| `--psgl-ffp-library <state.txt> -o <shaders.bin>` | Build a PSGL fixed-function shader library (see [below](#psgl-fixed-function-shader-library)). Must be the first argument. |
| `--general-lowering` | Accepted and ignored: the general lowering is the only back end. Kept so old scripts keep working; it will be removed. |
| `--legacy-lowering` | Refused with an explanation (exit 1): the shape-matching back end it selected has been removed. |

Any other argument that starts with `-` is refused as an `unknown option`
(exit 1) and the usage text is printed.

### Environment variables

| Variable | Effect |
|---|---|
| `SOURCE_DATE_EPOCH` | When set, `__DATE__` and `__TIME__` expand from this UTC timestamp instead of the clock, for reproducible builds. Must be a non-negative base-10 integer no greater than 253402300799 (end of year 9999); anything else is an error. |
| `RSXCG_GENERAL` | `RSXCG_GENERAL=0` used to select the removed back end; it is now refused with an explanation. Unset it. Any other value is ignored. |
| `RSX_DUMP_ORDER` | Debugging aid for compiler developers: when set, the general lowering prints its instruction-ordering decisions to stderr. Output files are unchanged. |

### Exit status

| Status | Meaning |
|---|---|
| 0 | Success (or `--help`, `--version`, `--list-extensions`). |
| 1 | Any compile error, refused option, unreadable input or unwritable output. No output file is written on a compile error. |
| 2 | `--psgl-ffp-library` with a malformed command line. |

## Profiles and language features

### Profiles

| Profile | Stage | Container | Usual source extension |
|---|---|---|---|
| `sce_vp_rsx` | vertex program | `.vpo` | `.vcg` |
| `sce_fp_rsx` | fragment program | `.fpo` | `.fcg` |

The compiler itself does not look at the file extension; the profile comes
from `-p`. The build helpers and `scripts/shader-compile-check.py` pick the
profile from `.vcg`/`.fcg` (and the check script also from a `_v`/`_f`
name suffix on `.cg` files).

### Preprocessor

The preprocessor follows C: `#define` (object-like and function-like),
`#undef`, `#include "..."` and `#include <...>`, `#if`/`#ifdef`/`#ifndef`/
`#elif`/`#else`/`#endif` with `defined`, `#line`, `#error`, `#pragma`, and
backslash line continuation.

- `#include "file"` searches the directory of the including file first, then
  each `-I` directory in order. `#include <file>` searches only the `-I`
  directories. `#pragma once` is honoured.
- Predefined macros: `__LINE__`, `__FILE__`, `__DATE__`, `__TIME__` (these
  expand but do not count as defined for `#ifdef`/`defined`), and two
  compiler-identification macros with the value `20000`, `__CGC__` and its
  vendor-prefixed twin, matching the established compiler. `__STDC__` and
  `__STDC_VERSION__` are not defined.
- **There is no command-line `-D` option.** Put configuration macros in the
  source or in a header selected with `-I`, or generate a small header from
  your build system.

Pragmas:

| Pragma | Behaviour |
|---|---|
| `#pragma once` | Include guard for the current file. |
| `#pragma alphakill <sampler>` | Fragment programs: the runtime discards fragments where the named sampler returns alpha 0. Recorded in the `.fpo` as a synthetic per-sampler parameter in declaration order, and fetches from alpha-kill samplers are never removed as dead code. |
| anything else | Accepted and ignored (`pack_matrix`, `loop`, `branch`, `argument`, `position_invariant`, `warning` and unknown pragmas pass through without effect). |

### Language

The accepted language is Cg as the established compiler accepts it for the
two RSX profiles:

- Scalar, vector and matrix types (`float`, `half`, `fixed`, `int`, `bool`,
  `floatN`, `floatRxC`, ...), structs (including nested structs and struct
  inputs/outputs on the entry function), arrays, typedefs, `static` and
  `const` globals, `uniform` parameters and globals.
- Functions with `in`, `out` and `inout` parameters, overloading, default
  arguments on entry parameters; helper functions are inlined.
- Control flow: `if`/`else`, `for`, `while`, `do`, `break`, `continue`,
  `return`, `discard` (fragment), ternary and comma operators.
- The Cg standard library through the embedded header: arithmetic,
  trigonometric and exponential functions, `lerp`, `saturate`, `clamp`,
  `dot`, `cross`, `normalize`, `reflect`, `refract`, `faceforward`, `mul`,
  `transpose`, `determinant`, `ddx`/`ddy`, pack/unpack, and texture
  functions such as `tex1D`, `tex2D`, `tex2Dproj`, `tex2Dlod`, `tex3D`,
  `texCUBE` and `texRECT`.
- Samplers: `sampler1D`, `sampler2D`, `sampler3D`, `samplerCUBE`,
  `samplerRECT`, as uniform parameters, globals or arrays (fragment).

Semantics recognised on entry parameters include, for vertex programs,
`POSITION`, `NORMAL`, `COLOR`/`DIFFUSE`/`SPECULAR`, `TEXCOORD0..7`, `TANGENT`,
`BINORMAL`, `BLENDWEIGHT`, `BLENDINDICES`, `ATTRn` and `FOG` inputs, and
`POSITION`/`HPOS`, `COLOR0/1`, `TEXCOORD0..7`, `FOG`/`FOGC`, `PSIZE` and
clip-distance outputs; for fragment programs, `COLOR0/1`, `TEXCOORD0..7`,
`FOG`, `WPOS` and `FACE` inputs, and `COLOR0..3` (multiple render targets) and
`DEPTH`/`DEPR` outputs. Inputs without a semantic get implicit `TEXCOORD`
(fragment) or `ATTR` (vertex) bindings in the order the established compiler
uses, counting only the inputs the program still reads. Explicit uniform
register bindings (`: C<n>`, `: TEXUNIT<n>`) are honoured.

A vertex program must write `POSITION`/`HPOS` on some path; otherwise it is
refused with `error C6014: Required output 'HPOS' not written`.

### Extensions

Every extension is off by default, so an unflagged compile accepts and emits
what the established compiler does. When a source uses an extension-only
construct without the flag, the diagnostic names the exact flag to add.
`rsx-cg-compiler --list-extensions` prints the current list:

| Name | Enables |
|---|---|
| `bom` | One leading UTF-8 byte order mark (`EF BB BF`) in the main source and in `#include` files. Without it the BOM is refused. |
| `utf16` | A source whose first two bytes are a UTF-16 byte order mark (`FF FE` little endian, `FE FF` big endian). The whole file is transcoded to UTF-8 first. An odd byte count, an unpaired surrogate, or a U+0000 after the mark is still refused with a diagnostic naming the defect. UTF-32 marks are always refused. |
| `declarator-types` | In `float3 a = ..., b = ...;` every declarator keeps the declared type. Without it, a later declarator takes the type last named in an earlier declarator's initializer, as in the established compiler. |
| `static-parameters` | `static`, `static const` and `const static` on helper-function parameters (`static` is ignored, `const` stays read-only). `static` on the selected entry's parameters is still refused. |
| `glsl-functions` | `fract`, `mix` and floor-remainder `mod` on numeric scalars and vectors. Functions declared in the source take priority; `mix` with a boolean selector selects. |
| `glsl-types` | GLSL type spellings as aliases of Cg types (table below). |

`glsl-types` spellings:

| Spelling | Cg type |
|---|---|
| `vec2`, `vec3`, `vec4` | `float2`, `float3`, `float4` |
| `ivec2` ... `ivec4` | `int2` ... `int4` |
| `bvec2` ... `bvec4` | `bool2` ... `bool4` |
| `dvec2` ... `dvec4` | `double2` ... `double4` (represented as float) |
| `mat2`, `mat3`, `mat4` | `float2x2`, `float3x3`, `float4x4` |
| `matRxC`, R and C from 2 to 4 | `floatRxC` |

These are aliases inside a Cg program: matrix indexing stays Cg row
indexing (`mat2x3` has two rows of three components), a scalar matrix
constructor fills every component, and GLSL column-major construction,
diagonal scalar construction and 64-bit arithmetic are not implemented.
Source declarations with the same names win, and the names are not reserved
keywords.

## Outputs

### Program container (`--emit-container`)

The default container is the binary Cg program layout the GCM runtime
expects: a big-endian header starting with the profile word (`0x00001b5b`
vertex, `0x00001b5c` fragment), followed by the parameter table (names,
semantics, resources, default values, referenced flags), the
program-specific attribute block (input/output masks, register count,
texture-coordinate controls, ...) and the microcode.

- Fragment microcode has no constant registers: every literal and every use
  of a uniform is an inline 16-byte constant block after the instruction that
  reads it, and the parameter table lists the microcode offsets the runtime
  patches when a fragment uniform is set.
- Vertex uniforms live in constant registers; the container records each
  parameter's register and default values.
- Unused parameters are stripped from the table, as by the established
  compiler.

`cellGcmCgInitProgram`, `cellGcmCgGetUCode`, `cellGcmCgGetNamedParameter`
and the rest of the `cellGcmCg*` helpers in `libgcm_cmd`, and PSGL's
`cgCreateProgram` with `CG_BINARY`/`CG_OBJECT`, consume this layout. The
PSL1GHT `rsxLoadVertexProgram`/`rsxLoadFragmentProgramLocation` path does not:
it needs `cgcomp`'s layout (see [cgcomp compatibility](#cgcomp-compatibility)).

### Compact CGB container (`--emit-cgb-container`)

A smaller runtime-facing container, starting with the magic `CGB\0`, that
carries the same microcode with a compact parameter list (register indices
and their float4 values, sampler units). PSGL's `cgCreateProgram` accepts it.
Known defect: a program created from a compact CGB **fragment** container
currently draws nothing; use the regular container for production.

### Microcode dump

With no output option the compiler prints, to stdout:

```
Entry point: main
Stage: fragment
IR functions: 1
NV40 ucode words: 8
    0: 9e001700 a8011c9c c8000001 c8003fe1
    1: 9e010200 c8001c9d c8010001 c8003fe1
```

one row per 16-byte instruction slot (fragment inline constant blocks are
printed as rows too). The `Entry point` / `Stage` / `IR functions` lines are
also printed when a container is written; build systems can ignore stdout.

### Shader archive packer

The second executable built from `tools/rsx-cg-compiler/CMakeLists.txt`
(installed next to `rsx-cg-compiler`; its source is under
`tools/rsx-cg-compiler/src/` in its own subdirectory) packs one container or
a folder of containers into one big-endian ELF32 relocatable shader archive:
one symbol per program named after its input file, a shader table, each
program's microcode in its own `.text` section, and a parameter table per
program rebuilt as a tree from the container's flattened parameter names.

```
<packer> [-q|--quiet] [-s|--no-sem] [-e] input [output]
```

| Option | Effect |
|---|---|
| `-q`, `--quiet` | Print nothing on success (also silences unknown-option warnings). |
| `-s`, `--no-sem` | Omit parameter semantics. |
| `-e` | Keep the file extension in symbol names. |
| `-u`, `--no-unref` | Refused (exit 1): dropping unreferenced parameters is not supported. |
| `-v`, `-V`, `--version` | Print the version (same as the compiler's) and exit. |
| `-h`, `--help` | Print help and exit. |

`input` is one container or a folder; a folder converts every regular file
in case-insensitive name order and skips files that are not shader
containers. Without `output` the archive is written to
`<dir of input>/out<extension of input>`. The archive is written to a
temporary file and renamed into place, so a failure never leaves a partial
archive. Unknown options are ignored with a warning.

## PSGL fixed-function shader library

PSGL's fixed-function pipeline draws with precompiled vertex/fragment
program pairs, one pair per fixed-function state mask. An application
loads them with `psglLoadShaderLibrary`. `rsx-cg-compiler` builds that
library:

```
rsx-cg-compiler --psgl-ffp-library state.txt -o shaders.bin
```

`--psgl-ffp-library` must be the first argument. After it come exactly one
state file and `-o <output>`, in either order; anything else is a usage
error (exit 2).

**State file.** One state mask per line, written as any C integer literal
(`0x00000001`, `1`, `010`); `#` starts a comment, and blank lines and
surrounding whitespace are ignored. Masks are the PSGL fixed-function keys
from `sdk/libPSGL/src/ffp_emit.h`:

| Mask | Program pair |
|---|---|
| `0x00000001` | textured, texture unit 0 (the minimal mask) |
| `0x000N0000`, N = 1..15 | textured, one bit per enabled texture unit 0..3 (bit 16 + unit) |
| `0x00000002` set | lighting; the fog (`0x4`) and texture-unit bits must be clear |
| `0x00000008` | position only (must be the whole mask) |

Example (`samples/toolchain/hello-psgl-ffp-shaderlib/state.txt`):

```
# Minimal textured fixed-function mask.
0x00000001
```

**What it generates.** For each mask, the vertex and fragment Cg come from
libPSGL's own generator (`sdk/libPSGL/src/ffp_emit.c`, compiled into the
compiler, so the library and the masks libPSGL looks up cannot disagree).
Both programs are compiled in process; each is byte-for-byte the container
`-p sce_vp_rsx --emit-container` / `-p sce_fp_rsx --emit-container` would
write for that source. The output file, all integers big-endian:

```
"PSGLSHDR"                  8 bytes
u32 version                 PSGL_SHADER_LIBRARY_VERSION (currently 1)
u32 count
count times:
    u32 mask
    u32 vertex container size
    u32 fragment container size
    vertex container bytes
    fragment container bytes
```

A malformed line (`invalid state line`), a mask the generator does not
support (`unsupported mask 0x...`) or a failed compile (`mask 0x... failed
to compile`) exits 1 and writes nothing.

CMake example (from the sample):

```cmake
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/shaders.bin"
    COMMAND "${PSGL_RSX_CG_COMPILER}" --psgl-ffp-library
            "${CMAKE_CURRENT_SOURCE_DIR}/state.txt"
            -o "${CMAKE_CURRENT_BINARY_DIR}/shaders.bin"
    DEPENDS "${PSGL_RSX_CG_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/state.txt"
    VERBATIM)
ps3_bin2s(my-app "${CMAKE_CURRENT_BINARY_DIR}/shaders.bin")
```

The sample also checks that the compiler's `--help` lists
`--psgl-ffp-library` and stops with a clear message when it does not.

### Migrating from psgl-shader-builder

`psgl-shader-builder` used to build the same library by running the
compiler twice per mask through temporary files. It is now a deprecated
wrapper that runs `--psgl-ffp-library`, prints a deprecation notice, and
will be removed. The work directory argument is no longer used.

| Old | New |
|---|---|
| `psgl-shader-builder <rsx-cg-compiler> state.txt shaders.bin [work-dir]` | `rsx-cg-compiler --psgl-ffp-library state.txt -o shaders.bin` |

The state file format and the output file are unchanged.

## Inspection and dump tools

### Preprocessed source

```
rsx-cg-compiler -p sce_fp_rsx --dump-preprocess --no-stdlib shader.fcg
```

prints the macro-expanded text with `#line` markers and stops. Use it to
check include resolution, conditional compilation and macro expansion.

### Syntax tree and IR

`--dump-ast` prints the parsed tree; `--dump-ir` prints the IR as it reaches
the back end (after inlining, simplification, dead-code removal, discard
guards, if-conversion and implicit binding):

```
-- IR -----------------------------------------------------------
; Module: t.fcg
; Stage: fragment
; Entry: main

define void @main(vec4 %c : TEXCOORD, sampler2D %s, vec4 %o : COLOR) entry {
entry:
  %4 = shuffle vec2 %1 [xy]
  %5 = sample vec4 %2, %4
  %6 = mul vec4 %5, %1
  stout %6 COLOR
  ret
```

Both continue compiling, so they can be combined with `--emit-container`.

### Program statistics

`--stats` prints one line on stderr after a successful compile, without
changing any output:

```
rsx-cg-compiler stats: fp instructions=N inline_const_blocks=N uniform_patch_slots=N uniforms_embedded=N registers=N kil=N ucode_bytes=N
rsx-cg-compiler stats: vp instructions=N ucode_bytes=N
```

For a fragment program: instruction slots, inline constant blocks, the
microcode slots the runtime patches each time a uniform is set, embedded
uniforms, the register count, `KIL` instructions, and microcode size (16
bytes per instruction slot and per constant block). For a vertex program:
instructions and microcode size. Collect it in a normal build to see what
each shader feature costs.

### Microcode decoders (test helpers)

The shader-compiler tests ship small Python decoders that are also handy by
hand. They are independent and use only the NV40 field definitions in
`src/nv40/nvfx_shader.h`:

| Script | Input | Output |
|---|---|---|
| `tests/shader-compiler/ucode_decode.py <dump.txt>...` | the stdout of a compile without an output option | one line per fragment instruction: opcode, precision, condition-code write/test, destination, write mask, input and constant operand counts, end bit |
| `tests/shader-compiler/vp_words.py <container>` | a `.vpo` | the vertex instructions, one line each, restricted to measured fields |
| `tests/shader-compiler/uniform_container_check.py <container>` | a `.vpo`/`.fpo` | JSON report cross-checking the uniform metadata against the operands the microcode actually encodes; exit 1 on issues, 2 on a malformed container |
| `tests/shader-compiler/uniform_container_census.py <compiler> --output <csv>` | a compiler | runs the uniform check over a corpus and writes a census |

There is no full disassembler yet; microcode-level round-trip checking is
planned.

## Comparison and regression tooling

The compiler has no built-in comparison mode. Comparison is done by scripts
in the repository that compile the same sources with two compilers, or with
one compiler under two settings, and compare the results. A second compiler,
when a tool takes one, is always an executable you supply on your machine;
its outputs are runtime inputs and never enter the tree.

### Compile gate: `scripts/shader-compile-check.py`

Compiles every shader in a corpus and reports what fails.

```
scripts/shader-compile-check.py --out build/shader-check
scripts/shader-compile-check.py --out build/shader-check \
    --external-corpus build/shader-corpus --min-shaders 100
```

| Option | Effect |
|---|---|
| `--out <dir>` | Required. Output directory; `results.csv` gets one row per shader (what was tried, whether it compiled, container size, first diagnostic). |
| `--ours <path>` | Compiler to test. Default: `rsx-cg-compiler.exe` or `rsx-cg-compiler`, as a path or on `PATH`. |
| `--corpus <dir>` | Repo-relative root of checked-in shaders (repeatable). Default: `tools/rsx-cg-compiler/tests/shaders` and `samples`. |
| `--external-corpus <dir>` | Root of a fetched corpus, walked instead of git-listed (repeatable). |
| `--min-shaders <n>` | Fail if fewer shaders were found (default 1), so an empty corpus never passes. |
| `--general-lowering` | Accepted and ignored. |

The profile is taken from `.vcg`/`.fcg` or a `_v`/`_f` stem suffix; a shader
whose stage cannot be decided is reported, not guessed.

### Community corpus: `scripts/fetch-shader-corpus.sh`

Fetches public Cg shader collections at pinned commits into a gitignored
directory (default `build/shader-corpus`) as test inputs only.

```
scripts/fetch-shader-corpus.sh --list           # source / license / count table (CSV)
scripts/fetch-shader-corpus.sh --dry-run        # validate metadata, no network
scripts/fetch-shader-corpus.sh --check-root DIR # is DIR a safe destination?
scripts/fetch-shader-corpus.sh [--all | --source <id>] [--root DIR]
```

GPL and ambiguous-license shaders are fetched for running only and are never
committed. The license policy is in `docs/design/shader-compiler-testing.md`.

### Shader test suite: `tests/shader-compiler/`

About 400 focused tests (`*-test.sh` plus Python and C++ checkers), each
taking the compiler path as its argument, for example:

```
bash tests/shader-compiler/vp-lerp-test.sh tools/rsx-cg-compiler/build/rsx-cg-compiler
```

CI runs the list in `.github/workflows/ci.yml`.
`tests/shader-compiler/suite-runner.py` runs that same list locally with
progress output and idle/wall timeouts:

| Option | Effect |
|---|---|
| `--compiler <path>` | Compiler under test. |
| `--suite all\|shader-compiler` | Which tests (default `all`). |
| `--workflow <ci.yml>` | Workflow file to read the list from. |
| `--filter <regex>` | Select tests by path. |
| `--fail-fast` | Stop at the first failure or timeout. |
| `--idle-timeout <s>`, `--wall-ceiling <s>`, `--timeout <s>` | Kill a test that stops making progress (default idle 300 s, wall 600 s). |
| `--scratch-root <dir>`, `-v` | Scratch location, verbose output. |
| `--self-check` | Test the runner itself. |

Results are PASS, FAIL, TIMEOUT, UNRUNNABLE or SKIPPED; the run fails on any
failure, timeout, unrunnable test or empty selection.

### Differential rig: `tests/regression/shader-differential/`

A runtime A/B rig that judges **pixels**, not bytes. It renders two fragment
containers side by side into off-screen render targets on RPCS3 (or a
console) and compares the readback raw. Pairs can be:

- the same shader compiled by this compiler with fast-math on and off
  (`stage-corpus.sh`, run where the compiler lives; only byte-differing pairs
  are staged, verdict changes between the two settings are reported as
  findings);
- this compiler's container against a container from a second compiler you
  supply (by parameter or environment variable; see the script headers);
- built-in control pairs that prove the rig can tell identical from
  different output (MRT, depth, sampler kinds), always run first.

Main pieces:

| Script | Role |
|---|---|
| `stage-differential.ps1` | Compiles the controls and pairs, writes the manifest and stages everything into RPCS3's `dev_hdd0`. |
| `stage-corpus.sh <compiler> <corpus-root> <out-dir>` | Corpus half of the stager (fast/nofast pairs). Refuses a zero-compile run. |
| `must-reject.ps1` | Holds every case in `must-reject.txt` to its listed verdict: `accept`, `frontend-reject` (diagnostic names `file:line:col`) or `backend-refuse`. Prints `MUST_REJECT_OK` / `MUST_REJECT_FAIL`. |
| `n-live.py` | Generates a family of fragment shaders with N live terms and reports the first N each compiler refuses and the instructions per term (register-pressure measure; compile only). |
| `container-metrics.ps1` with `container-metrics-baseline.csv` | Reads instruction and register counts from the containers of each pair and fails staging when a row gets worse than its baseline; `-MetricsReportOnly` reports without failing. Policy: `container-metrics-policy.txt`. |
| `current-census.ps1`, `current-gap-summary.ps1` | Bucket accepted/refused shaders across two compilers into a census CSV and summarise the remaining gaps. |
| `f32_compare.py A.raw B.raw` | Bit-exact comparison of two float render-target readbacks (JSON verdict; NaN and signed-zero aware). |

### PSL1GHT cgcomp

Where NVIDIA's Cg runtime is available, PSL1GHT's `cgcomp` can also be run
on the same sources as a community baseline; it is not required for
anything.

## Build integration

### CMake

`include(ps3-self)` (in `cmake/ps3-self.cmake`) provides:

```cmake
ps3_add_cg_shader_rsxcgc(<target> <file>)
```

- `<file>` is relative to the current source directory (or absolute) and must
  end in `.vcg` (compiled with `-p sce_vp_rsx`) or `.fcg` (`-p sce_fp_rsx`);
  any other extension is a configure error.
- The container is written to `${CMAKE_CURRENT_BINARY_DIR}/shaders/<stem>.vpo`
  or `.fpo` with `--emit-container`, and embedded in `<target>` with
  `ps3_bin2s`. Your code sees the bin2s symbols `<stem>_vpo` / `<stem>_fpo`
  (plus `_end` and `_size`) and a header `<stem>_vpo.h` / `<stem>_fpo.h`.
- Extra flags: set the CMake list `PS3_RSXCGC_FLAGS` (empty by default), for
  example `-DPS3_RSXCGC_FLAGS="--extension=bom;-O2"`. It applies to every
  shader in the project.
- The compiler is searched for as `PS3_TOOL_rsxcgc` in `$PS3DEV/bin`,
  `$PS3DK/bin` and `tools/rsx-cg-compiler/build`; a missing compiler is a
  configure error when the function is called.

```cmake
include(ps3-self)
add_executable(my-app source/main.c)
ps3_add_cg_shader_rsxcgc(my-app shaders/vpshader.vcg)
ps3_add_cg_shader_rsxcgc(my-app shaders/fpshader.fcg)
ps3_add_self(my-app)
```

```c
#include "vpshader_vpo.h"
#include "fpshader_fpo.h"
CGprogram vp = (CGprogram)vpshader_vpo;
CGprogram fp = (CGprogram)fpshader_fpo;
cellGcmCgInitProgram(vp);
cellGcmCgInitProgram(fp);
```

For anything the helper does not cover (per-shader flags, `-I`, a different
entry, the compact container), call the compiler with `add_custom_command`
yourself, as the PSGL samples under `samples/toolchain/` do.

### Makefiles

The pattern used by `sdk/libdbgfont/Makefile`:

```make
RSX_CG ?= rsx-cg-compiler
BIN2S  ?= bin2s

build/%.vpo: shaders/%.vcg
	$(RSX_CG) -p sce_vp_rsx --emit-container $@ $<

build/%.fpo: shaders/%.fcg
	$(RSX_CG) -p sce_fp_rsx --emit-container $@ $<

build/%.vpo.s: build/%.vpo
	$(BIN2S) $< > $@

build/%.fpo.s: build/%.fpo
	$(BIN2S) $< > $@
```

then assemble the `.s` files and link the objects. Name the container after
the symbol you want: bin2s turns `vpshader.vpo` into `vpshader_vpo`.

### cgcomp compatibility

PSL1GHT's `cgcomp` is still installed for projects written against it, and
`ps3_add_cg_shader(<target> <file>)` still drives it. The two compilers
write **different container layouts**, so pick the compiler by the runtime
API that loads the shader:

| Runtime API | Compiler | CMake helper |
|---|---|---|
| `cellGcmCg*` (`cellGcmCgInitProgram`, `cellGcmSetVertexProgram`, ...), PSGL `cgCreateProgram` | `rsx-cg-compiler` | `ps3_add_cg_shader_rsxcgc` |
| PSL1GHT `rsxLoadVertexProgram`, `rsxLoadFragmentProgramLocation` | `cgcomp` | `ps3_add_cg_shader` |

Passing a `cgcomp` blob to the `cellGcmCg*` helpers faults (they read the
microcode offset from a header field `cgcomp` does not have), and
`rsx-cg-compiler` has no option to write the `cgcomp` layout. Moving a
PSL1GHT project to `rsx-cg-compiler` therefore means moving its shader
loading to `cellGcmCg*` at the same time.

Command-line mapping:

| cgcomp | rsx-cg-compiler |
|---|---|
| `cgcomp -v in.vcg out.vpo` | `rsx-cg-compiler -p sce_vp_rsx --emit-container out.vpo in.vcg` |
| `cgcomp -f in.fcg out.fpo` | `rsx-cg-compiler -p sce_fp_rsx --emit-container out.fpo in.fcg` |

The bin2s symbol names are the same for both (`<stem>_vpo`, `<stem>_fpo`).
A future release will need an explicit build flag to install `cgcomp` at
all.

## Diagnostics

Messages go to stderr. Source diagnostics use the usual form

```
shader.fcg:12:5: error: <message>
shader.fcg:3:1: warning: <message>
```

and many carry the established compiler's error code (for example `C1047`
redefinition of a struct, `C5122` declarations, `C6014` missing vertex
position output) so messages can be matched across compilers. Each stage
ends a failed compile with a summary line:

| Summary | Stage |
|---|---|
| `rsx-cg-compiler: N parse error(s).` | parser |
| `rsx-cg-compiler: semantic analysis failed (N error(s)).` | type checking, overloads, entry point lookup |
| `rsx-cg-compiler: IR generation failed.` | IR builder |
| `NV40 discard guards failed: bailing out.` | discard lowering |
| `NV40 emit failed: bailing out.` | back end; the lines before it start with `nv40-general:`, `nv40-general-fp:` or `nv40-general-vp:` and name what could not be lowered |
| `rsx-cg-compiler: container emit failed for <file>` | container writer (`cg-container-vp:` / `cg-container-fp:` lines precede it) |

Other messages: `cannot open <file>`, `Cannot find include file: <name>`,
`unknown option`, `unknown profile`, `unknown extension '<name>'; supported:
...`, `only one input file allowed`, `--emit-container and
--emit-cgb-container are mutually exclusive`, `cannot open <path> for
writing`, `write failed`.

The back end **refuses** rather than guessing: a shape it cannot lower
correctly is an error with a reason (for example `vertex sampler arrays are
not lowered; refusing`, `unsupported output semantic`, `vp-varying-matrix-input`),
never silently wrong microcode. A refusal that names the source position comes
from the front end; one that does not comes from the back end.

## Known limits

- **Optimisation flags have no effect yet** (see [Optimisation](#optimisation)).
  The fast/nofast pairs in the differential rig exist to catch the day they
  do.
- **No `-D`**, and `-I` takes its directory as a separate argument.
- **`#pragma texformat` is not implemented**; it is accepted and ignored like
  other unknown pragmas.
- **Register budgets.** A fragment program must fit in 48 temporary
  registers and a vertex program in 32; a program past the budget is refused
  with a `fragment-output-liveness` or `vp-register-budget` diagnostic.
  Shaders that keep many values live at once (for example long unrolled sums
  of independently computed terms) can reach it sooner than the established
  compiler does; splitting the work or reducing live values helps.
- **Vertex texture fetch:** four texture units; a fifth used sampler (C6012)
  or a used sampler bound to `TEXUNIT4` or above (C5102) is refused.
- **Vertex programs:** sampler arrays and varying matrix inputs are refused;
  integer/bool-to-float conversion of runtime values is refused.
- **Compact CGB fragment containers** load in PSGL but currently draw nothing.
- **No PSL1GHT `cgcomp` layout output.**
- **Mismatched `float` actuals for `out fixed` parameters** are accepted when
  they should be refused (see `docs/known-issues.md`).
- **No full microcode disassembler**; see the decoders above.
- **Code-generation switches not on the command line:** back-face colour
  output bits, keeping unused parameters, NRMH use, early kill scheduling,
  the point-size clamp workaround, perspective-correction control, signed
  texture remapping, and inlining/unrolling/if-conversion policies have no
  flags. The compiler uses fixed behaviour that matches the established
  compiler's defaults (front-face colour only, unused parameters stripped).

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `error C6014: Required output 'HPOS' not written` on a fragment shader | No `-p` was given, so it compiled as a vertex program. Add `-p sce_fp_rsx`. |
| `unknown option '-Ifoo'` | Write `-I foo`. |
| `unknown option '-DNAME=1'` | There is no `-D`; define the macro in a header. |
| A file with a byte order mark is refused | The diagnostic names the flag: add `--extension=bom` or `--extension=utf16`, or save the file as plain UTF-8. |
| `cellGcmCgGetUCode` faults | The blob came from `cgcomp`; use `ps3_add_cg_shader_rsxcgc`. |
| `rsxLoadFragmentProgramLocation` renders garbage | The blob came from `rsx-cg-compiler`; that API needs `cgcomp`, or switch to `cellGcmCg*`. |
| `RSXCG_GENERAL=0 selected the legacy NV40 shape matcher, which has been removed` | Unset `RSXCG_GENERAL`. |
| `--legacy-lowering selected the NV40 shape matcher` | Drop the flag. |
| `... temp registers ... refusing (fragment-output-liveness)` | Too many values live at once; see [Known limits](#known-limits). |
| `ps3_add_cg_shader_rsxcgc: rsx-cg-compiler not found` | Install the SDK host tools (`scripts/install-host-tools.sh`) or build `tools/rsx-cg-compiler/build`. |
| `__DATE__` differs between builds | Set `SOURCE_DATE_EPOCH`. |
| `--stats` is an unknown option | The installed compiler predates it; update the SDK. |
| The PSGL sample says the compiler does not support `--psgl-ffp-library` | It found an older compiler; set `PSGL_RSX_CG_COMPILER` to the one from the same SDK. |

## For contributors

- Option parsing is in `tools/rsx-cg-compiler/src/main.cpp`; the option
  record is `rsx_cg::CompileOptions` in `src/compile_options.h`, passed to
  every NV40 emit entry point (`emitVertexProgram[Ex]`,
  `emitFragmentProgram[Ex]` in `src/nv40/nv40_emit.h`).
- A pass that only makes code faster gates on the level and fast-math
  (`rsx_cg::atLeast(opts.opt, rsx_cg::OptLevel::O2) && opts.fastmath`). A
  pass that changes behaviour gets its own `CompileOptions` field. Levels
  `-O0` to `-O3` keep the established compiler's meaning; improvements past
  it go under `-O4`.
- Extensions are registered in `src/extensions.{h,cpp}`. Each is off by
  default, has exactly one table row, and must reduce to a construct the
  unflagged compiler already accepts; its disabled-mode diagnostic names the
  enabling flag.
- Every change needs a test under `tests/shader-compiler/` and a row in
  `.github/workflows/ci.yml` (`tests/ci-test-list-test.py` checks the list).
  Test shaders live in `tools/rsx-cg-compiler/tests/shaders/`.
- The corpus policy, pass criteria and harness design are in
  `docs/design/shader-compiler-testing.md`.
