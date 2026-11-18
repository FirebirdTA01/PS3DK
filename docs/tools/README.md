# Tool guides

Every tool the SDK installs, and where its guide is. On Linux the host tools are
in `$PS3DEV/bin` (mirrored to `$PS3DK/bin`); in the Windows release they are
`.exe` files in `%PS3DK%\bin`. The cross-compilers are under `ppu/bin` and
`spu/bin`.

## Compilers, binutils and build integration

| Guide | Covers |
|---|---|
| [toolchain.md](toolchain.md) | PPU and SPU GCC, binutils, PPU GDB, the ILP32 default and `-mlp64`, multilibs, flags, environment variables, `setup.cmd`, the short `ppu-*` / `spu-*` names, the CMake toolchain files, the `ps3-self` helpers (`ps3_add_self`, `ps3_bin2s`, shaders), and building without CMake |
| [../building.md](../building.md) | Building the toolchain and SDK from source, WSL2, and the Windows release cross-build |

## Host tools

| Tool | Guide | What it is for |
|---|---|---|
| `rsx-cg-compiler` and its shader archive packer | [rsx-cg-compiler.md](rsx-cg-compiler.md) | Cg vertex and fragment shader compiler for the RSX (the recommended shader compiler) |
| `sfo`, `sfo-gui` | [sfo.md](sfo.md) | Create, inspect, validate and edit `PARAM.SFO` |
| `pkg`, `make_self_npdrm`, `package_finalize` | [pkg.md](pkg.md) | Installable `.pkg` packages and the `ps3_add_pkg` CMake helper |
| `prx-gen` | [prx-gen.md](prx-gen.md) | Build and check loadable PRX / SPRX modules; `ps3_add_prx` and importers |
| `nidgen` | [nidgen.md](nidgen.md) | NIDs, export lists and import stub archives |
| `sprxlinker` | [sprx-linker.md](sprx-linker.md) | Post-link fix-up of an executable's import tables, before signing |
| `spu-elf-to-ppu-obj` | [spu-elf-to-ppu-obj.md](spu-elf-to-ppu-obj.md) | Wrap an SPU program into a PPU object; `ps3_add_spu_image` |
| `make_sdata` | [make-sdata.md](make-sdata.md) | SDATA files for `cellFsSdataOpen`; `ps3_make_sdata` |
| `cellOvisConfigAuto`, `cellOvisMkLdscript` | [ovis.md](ovis.md) | SPU overlay support |
| `abi-verify` | [abi-verify.md](abi-verify.md) | Check PPU objects against the ABI contract |
| `coverage-report` | [coverage-report.md](coverage-report.md) | SDK export coverage matrix ([../coverage.md](../coverage.md)) |
| `make_self`, `fself`, `bin2s` | [toolchain.md](toolchain.md#the-ps3-self-helper-module) | SELF signing and data embedding, driven by `ps3_add_self` and `ps3_bin2s` |
| `cgcomp` | [toolchain.md](toolchain.md#shaders) | Legacy PSL1GHT shader compiler, bundled for older Makefiles |

## Profiler

The profiler runtime and its PC tools have their own guide:
[../profiler-guide.md](../profiler-guide.md).

## Deprecated

- **`psgl-shader-builder`** is deprecated in favour of `rsx-cg-compiler`. It is
  kept as a wrapper for one release: it still accepts
  `psgl-shader-builder <rsx-cg-compiler> <state.txt> <shaders.bin> [work-dir]`
  (the work directory is ignored), prints
  `psgl-shader-builder: deprecated, use: rsx-cg-compiler --psgl-ffp-library <state.txt> -o <shaders.bin>`,
  runs that command and exits with its status. Use the replacement directly:

  ```sh
  rsx-cg-compiler --psgl-ffp-library state.txt -o shaders.bin
  ```

  See [rsx-cg-compiler.md](rsx-cg-compiler.md).

- **`tools/psgl/glsl_subset_to_cg.py`** is a sample helper, not a GLSL
  compiler. `glsl_subset_to_cg.py <vertex|fragment> <input.glsl> <output.cg>`
  checks the input for the tokens of one known vertex shader and one known
  fragment shader and writes a fixed Cg equivalent; it does not parse GLSL. Its
  only user is `samples/toolchain/hello-psgl-glsl-object`. Write shaders in Cg
  and compile them with [rsx-cg-compiler](rsx-cg-compiler.md).
