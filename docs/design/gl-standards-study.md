# Newer GL standards on PSGL and RSX: design study

Status: study only, nothing implemented. Written 2026-10-25 against `main` e791a391
(the v0.26.0 tag). It covers the shader-compiler changes already agreed for the next minor
release (a `--psgl-ffp-library` mode in `rsx-cg-compiler`, and a GLSL front end) and asks
the director to choose a target tier.

Labels used below:
- **[measured]**: read from our source at the cited line, or counted with a script.
- **[public]**: from public hardware documentation (nouveau/envytools register databases,
  PSDevWiki, the RPCS3 source, the D3D9 shader-model-3 specifications). Not yet checked on
  silicon by us.
- **[estimate]**: judgement.

## 1. What we ship today

### 1.1 API surface

- **The headers are OpenGL ES 1.x.** `sdk/include/GLES/gl.h:32` defines
  `GL_OES_VERSION_1_0`, and `glGetString(GL_VERSION)` returns `"1.1 PS3DK"`
  (`sdk/libPSGL/src/core_gl.c:50`). There is no `GLES2/gl2.h`. The ES 2.0-style shader
  entry points are declared as extensions in `GLES/glext.h` [measured].
- **Most entry points are real.** The libPSGL wrappers forward to `psgl_context_*` in
  `psgl_context.c` (4731 lines). The header comment of `core_gl.c:1-6`, which calls the file
  "GLES 1.1 core stubs", is stale.
- **Entry-point counts** [measured, heuristic: a body with no call and no state write counts
  as a no-op]:

  | File | Entry points | Forward or real | No-op |
  |---|---:|---:|---:|
  | `core_gl.c` | 107 | 86 | 21 |
  | `core_gl_ext.c` | 92 | 28 | 64 |
  | `gl_shader.c` | 34 | 30 | 4 |

- **Notable no-ops** [measured]:
  - **Texture environment:** `glTexEnv*` (`core_gl.c:395`) and `glTexGen*`
    (`core_gl_ext.c:151`), so the texture-combiner stage of the fixed-function path is not
    emulated.
  - **Multitexture and readback:** `glMultiTexCoord4f` (`core_gl.c:424`) and `glReadPixels`
    (`core_gl.c:497`).
  - **3D textures:** `glTexImage3D` and its family (`core_gl_ext.c:64`).
  - **Draw and raster state:** `glDrawRangeElements` (`core_gl_ext.c:124`) and
    `glPolygonMode` (`core_gl_ext.c:138`).
  - **Renderbuffers:** every renderbuffer call (`core_gl_ext.c:251`), so framebuffer objects
    take texture attachments only.
  - **Mipmaps:** `glGenerateMipmapOES` (`core_gl_ext.c:301`).
  - **Queries:** occlusion queries `glBeginQueryARB` and family (`core_gl_ext.c:314`).
  - **Synchronisation:** NV fences and SCE events.
- **Shaders are precompiled only.**
  - `glShaderSource` stores text (`gl_shader.c:227`), but `glCompileShader` succeeds only
    when a binary was supplied (`gl_shader.c:270-280`).
  - `glShaderBinary` accepts one format, `GL_SHADER_BINARY_FORMAT_CGB_PS3DK`
    (`gl_shader.c:256`; `GLES/glext.h:331`).
  - The runtime compiler `sceCgc*` is a link stub (`cgc_compiler.c:1`), so there is no
    on-target compilation [measured].
- **Fixed-function emulation is state-mask driven.** There are four mask bits: minimal,
  lighting, fog and position-only (`ffp_emit.h`). `psgl_ffp_state_mask_to_cg`
  (`ffp_emit.c:81`) emits a Cg vertex/fragment pair per mask, which `psgl-shader-builder`
  compiles with `rsx-cg-compiler` and packs into `shaders.bin` [measured].
- **Limits** [measured, `psgl_context.h:9-14`]:
  - 4 texture units;
  - 4 fixed-function vertex attributes and 16 generic attributes;
  - 8 lights;
  - 16 FBO objects;
  - 3 frame buffers.
- **Texture formats:** `psgl_texture_reference_format` (`psgl_context.c:1829-1847`) maps
  only `GL_RGBA` to A8R8G8B8, plus DXT1, DXT3 and DXT5, in linear or swizzled layout.
  Depth textures DEPTH16 and DEPTH24_D8 exist for depth handling (`psgl_context.c:209-218`).
  Only 2D targets are handled (`psgl_context.c:1126`, `:1500`, `:1564`) [measured].
- **Multisampling:** the SCE MSAA modes (none, 2x diagonal, 4x square centred, 4x square
  rotated) are accepted (`psgl_context.c:408-430`) [measured].
- **Buffers:** VBOs are implemented, with `glBufferData`, `glBufferSubData` and
  `glMapBuffer` forwarding to the context (`core_gl_ext.c:194-198`) [measured].

### 1.2 Shader compiler

- **Profiles:** `rsx-cg-compiler` accepts `sce_vp_rsx` and `sce_fp_rsx`, plus the PSP2
  aliases (`tools/rsx-cg-compiler/src/main.cpp:194-200`). The front end is the imported Vita
  Cg front end in `src/cg_frontend`; the NV40 back end is in `src/nv40` [measured].
- **Control flow:** fragment control flow lowers to predicated writes. Forward if/else is
  flattened; `discard` and run-time loops landed in v0.23.0/v0.24.0
  (`docs/design/shader-compiler-control-flow.md:3-5, 45-63`) [measured].
- **Register pressure** is not modelled (`docs/design/shader-compiler-register-pressure.md:18`)
  [measured].
- **GLSL:** `tools/psgl/glsl_subset_to_cg.py` (52 lines) only accepts sources that contain a
  fixed set of tokens and refuses the rest ("unsupported vertex GLSL subset"). It is a
  template matcher, not a compiler [measured].

## 2. RSX capability (NV40/G70 class)

| Area | Capability | Source |
|---|---|---|
| Vertex program | Up to 512 instructions; we refuse more (`sdk/include/cell/gcm/gcm_cg_bridge.h:243`). Constants addressed up to 512 vec4 (`vp_uniform_struct_layout.h:165`). Branches, loops and subroutine calls with an address register. Vertex texture fetch exists only for a few float formats. | [measured] + [public] |
| Fragment program | Shader-model-3 class. Fp32 and fp16 registers; fewer live registers mean more parallelism. Dynamic branching is costly and is if-converted by us. Derivatives (`ddx`/`ddy`) are available. | [public] + `shader-compiler-register-pressure.md:22-25` |
| Integers | No true integer ALU: integer types are emulated in float. Bit operations, integer division and 32-bit integer exactness do not exist. | [public] |
| Textures | 2D, 3D and cube. The `CELL_GCM_TEXTURE_*` set includes B8, A4R4G4B4, R5G6B5, A8R8G8B8, G8B8, D1R5G5B5, DXT1/23/45, DEPTH24_D8(_FLOAT), DEPTH16(_FLOAT), W16Z16Y16X16 float, W32Z32Y32X32 float and X32 float (`gcm_enum.h:259-281`). Filtering of float textures is limited. | [measured] + [public] |
| Render targets | Up to 4 colour targets (MRT1-3, `gcm_enum.h:200-202`). Float colour targets. MSAA as above. | [measured] |
| Impossible | Geometry, tessellation and compute stages; transform feedback; true integer and bitwise operations in shaders; texture buffer objects; instancing via a divisor (emulable only with constants/streams); sRGB is partial (gamma remap per channel). | [public] |

## 3. Tier mapping

Effort scale: S = days, M = 1-3 weeks, L = more than a month [estimate].

### 3.1 OpenGL ES 2.0 + GLSL ES 1.00

| Feature | RSX | Ours today | Effort |
|---|---|---|---|
| Programmable VS/FS, attributes, uniforms, varyings | feasible | Cg binaries only | M (GLSL front end) |
| GLSL ES 1.00 language (no integers needed in practice, loops with constant bounds) | feasible | none (template script) | M-L |
| On-target `glCompileShader` | feasible but large (ship the compiler on PPU) | stub | L, optional |
| FBO with texture and renderbuffer attachments | feasible | texture only | S-M |
| `glGenerateMipmap`, NPOT textures with clamp | feasible | no-op | S |
| Texture formats: RGB, RGBA, LUMINANCE, ALPHA, 565, 4444, 5551 | feasible (mapped to GCM formats) | RGBA and DXT only | S |
| Cube maps | feasible | 2D only | S-M |
| Depth/stencil, blending, scissor, viewport | feasible | implemented | — |

### 3.2 OpenGL ES 3.0 / GLSL ES 3.00

| Feature | RSX | Ours | Effort |
|---|---|---|---|
| Integer types and bit operations (`uint`, `<<`, `&`, `%`) | **impossible** exactly; emulable only within the 24-bit float mantissa | none | — (subset only) |
| Uniform buffer objects | emulable (copy to constants) | none | M |
| MRT (`gl_FragData`/`layout(location)`) | feasible, 4 targets | none in PSGL | S-M |
| 3D textures, 2D array textures | 3D feasible; arrays emulable (3D/atlas) | 3D no-op | M |
| Instancing | emulable (stream frequency/constants) | none | M |
| Transform feedback | **impossible** | — | — |
| Occlusion queries | feasible (GCM reports) | no-op | S |
| sRGB textures/targets | partial | none | M |

### 3.3 Desktop GL 2.1 / GLSL 1.20

This is close to the hardware's native generation. It adds over ES 2.0:
- **Display lists and immediate mode:** emulable on the CPU.
- **Full fixed-function:** including the texenv combiners (today a no-op), texgen and two-sided
  lighting.
- **Raster state:** `glPolygonMode` and `glReadPixels`.
- **Formats and queries:** 3D textures, point sprites, occlusion queries.

All are feasible, at S-M each [estimate].

### 3.4 GL 3.3 core / GLSL 3.30

GL 3.3 requires integer shader types, texture buffers, geometry shaders, transform feedback
and `gl_VertexID`/instancing semantics that the RSX cannot provide. **Conformance is
impossible.** A "GLSL 3.30 syntax subset" (layout qualifiers, `in`/`out`, and UBOs emulated as
constants) is possible but would mislead users about what runs.

## 4. Architecture options

1. **A GLSL front end inside `rsx-cg-compiler`** (agreed). Parse GLSL ES 1.00, and later the
   ES 3.00 subset, into the same IR the Cg front end in `src/cg_frontend` produces. The NV40
   back end, the optimizer, the tests and the CGB container output are then shared. Cg and
   GLSL differ mainly in front-end semantics (qualifiers, built-ins, implicit conversions), not
   in the IR. This replaces `glsl_subset_to_cg.py`.
2. **`--psgl-ffp-library`** (agreed). The fixed-function generator moves into the compiler, so
   one tool builds `shaders.bin`. Extending the state mask to texenv and texgen is the
   prerequisite for desktop GL 2.1 fixed-function.
3. **Runtime API layering.**
   - **(a)** Grow libPSGL into an ES 2.0 superset of its ES 1.1 API, as PSGL already mixes both.
   - **(b)** Write a separate `libGLESv2` over GCM.

   Recommendation: (a). It keeps one context and state model and one set of samples, and
   avoids two drivers fighting over GCM. The ES 2.0 headers (`GLES2/gl2.h`) would be added as a
   view over the same library.
4. **Test strategy.**
   - **Compiler:** the rsx-cg suite plus golden IR/ucode for each GLSL feature.
   - **Pixels:** the shader differential rig compares rendered output for Cg vs GLSL versions
     of the same shader.
   - **Runtime:** a GL ES 2.0 conformance subset as RPCS3 regression rows.
   - **Hardware:** a hardware pass through ps3dk-run protocol 2 for anything RPCS3 is known to
     get wrong. See the peer-local-store DMA result of 2026-10-25: emulator results are not
     the hardware.

## 5. Recommendation

Target **OpenGL ES 2.0 + GLSL ES 1.00 first**, done properly. Then add the parts of ES 3.0 the
RSX can do (MRT, 3D textures, occlusion queries, UBOs and instancing emulated) as PSGL
extensions, without claiming ES 3.0. Do not pursue GL 3.3. Phases:

1. **Next minor:**
   - `--psgl-ffp-library`, keeping the builder as a wrapper for one release;
   - fix the cheap ES 1.1/2.0 gaps: renderbuffers, `glGenerateMipmap`, RGB/LUMINANCE/565/4444
     formats, cube maps, `glReadPixels`;
   - correct the stale "stub" comments.
2. **Following minor:**
   - the GLSL ES 1.00 front end in `rsx-cg-compiler`, with full built-ins;
   - `GLES2/gl2.h` over libPSGL;
   - the conformance-subset RPCS3 rows.
3. **Later:**
   - fixed-function texenv/texgen in the state mask;
   - MRT, 3D textures and queries as extensions;
   - optional on-target compilation.

Decisions needed from the director:
1. Is the target ES 2.0 (recommended), or desktop GL 2.1 fixed-function as well?
2. Do we ship an on-target GLSL compiler, or keep precompiled binaries only?
3. Do we extend libPSGL (recommended) or add a separate `libGLESv2`?
4. May ES 3.0-style features ship as extensions under ES 2.0, with no ES 3.0 claim?
