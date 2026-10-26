# Newer GL standards on PSGL and RSX: design study

Status: study only, nothing implemented. Written 2026-10-25 against `main` e791a391
(the v0.26.0 tag). It covers the shader-compiler changes already agreed for the next minor
release (a `--psgl-ffp-library` mode in `rsx-cg-compiler`, and a GLSL front end) and asks
the director to choose a target tier.

Labels used below:
- **[measured]**: read from our source at the cited line, or counted with the script in
  Appendix A.
- **[in-tree public]**: a public hardware description vendored in our tree, cited by file.
- **[unverified]**: a commonly stated hardware limit that this study has not tied to an exact
  public source or to a test of ours.
- **[estimate]**: judgement.

Specifications cited:
- [ES2] OpenGL ES 2.0 full specification,
  https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf
- [GLSL-ES-1.00] OpenGL ES Shading Language 1.00,
  https://registry.khronos.org/OpenGL/specs/es/2.0/GLSL_ES_Specification_1.00.pdf

## 1. What we ship today

### 1.1 API surface

- **The headers are OpenGL ES 1.x.** `sdk/include/GLES/gl.h:32` defines
  `GL_OES_VERSION_1_0`, and `glGetString(GL_VERSION)` returns `"1.1 PS3DK"`
  (`sdk/libPSGL/src/core_gl.c:50`). There is no `GLES2/gl2.h`. The ES 2.0-style shader
  entry points are declared as extensions in `GLES/glext.h` [measured].
- **Most entry points have a body that forwards or does work.** The libPSGL wrappers forward
  to `psgl_context_*` in `psgl_context.c` (4731 lines). The header comment of
  `core_gl.c:1-6`, which calls the file "GLES 1.1 core stubs", is stale.

  | File | Entry points | Forwards / nontrivial body | No-op body |
  |---|---:|---:|---:|
  | `core_gl.c` | 107 | 86 | 21 |
  | `core_gl_ext.c` | 92 | 28 | 64 |
  | `gl_shader.c` | 34 | 30 | 4 |

  The counts come from the script in Appendix A. **Forwarding to a context function does not
  establish semantic completeness**: each forwarded call still needs spec-level tests.
- **Notable no-op bodies** [measured]:
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
  - `glShaderSource` stores text (`gl_shader.c:227`). Without a supplied binary,
    `glCompileShader` sets `compiled = GL_FALSE` with the log "runtime GLSL compilation
    unavailable" (`gl_shader.c:276-280`); it raises no GL error.
  - `glShaderBinary` accepts one format, `GL_SHADER_BINARY_FORMAT_CGB_PS3DK`
    (`gl_shader.c:256`; `GLES/glext.h:331`).
  - The runtime compiler `sceCgc*` is a link stub (`cgc_compiler.c:1`) [measured].
- **Fixed-function emulation is state-mask driven.** There are four mask bits: minimal,
  lighting, fog and position-only (`ffp_emit.h`). `psgl_ffp_state_mask_to_cg`
  (`ffp_emit.c:81`) emits a Cg vertex/fragment pair per mask, which `psgl-shader-builder`
  compiles with `rsx-cg-compiler` and packs into `shaders.bin` [measured].
- **Limits** [measured]:
  - `psgl_context.h:11`: `PSGL_MAX_TEXTURE_UNITS 4`, also reported as `GL_MAX_TEXTURE_UNITS 4`
    (`core_gl.c:66`).
  - `psgl_context.h:12`: `PSGL_MAX_VERTEX_ATTRIBS 4`. This also bounds the shader attribute
    path (`gl_shader.c:372, 555, 574, 596`).
  - 16 generic attributes in the context, 8 lights, 16 FBO objects and 3 frame buffers
    (`psgl_context.h:9-14`).
  - `GL_MAX_TEXTURE_SIZE 4096` (`core_gl.c:61`).
  - `GL_NUM_COMPRESSED_TEXTURE_FORMATS` returns 0 (`core_gl.c:74`), although DXT1/3/5 are
    accepted.
- **Texture formats** [measured]:
  - **Accepted at upload** (`psgl_texture_supported`, `psgl_context.c:1619-1633`):
    - RGBA, BGRA, RGB and BGR as `GL_UNSIGNED_BYTE`;
    - RGBA as 5551 and 4444;
    - RGBA and BGRA as 8888 and 8888_REV;
    - RGB as 565.
  - **How they are stored:** upload converts all of them to A8R8G8B8 (`psgl_pack_argb8` path,
    `psgl_context.c:1700-1760`; consumed by `tex_image_2d`, `psgl_context.c:3723`).
  - **Native/reference storage formats:** A8R8G8B8 and DXT1, DXT3 and DXT5, linear or swizzled
    (`psgl_texture_reference_format`, `psgl_context.c:1829-1847`).
  - **Render-target and depth storage:** `GL_ARGB_SCE` colour, `GL_DEPTH_COMPONENT16` and
    `GL_DEPTH_COMPONENT24` (`psgl_context.c:3715-3735`).
  - **Not accepted:** LUMINANCE, ALPHA and LUMINANCE_ALPHA.
  - **Targets:** only 2D is handled (`psgl_context.c:1126`, `:1500`, `:1564`).
- **Multisampling:** the SCE MSAA modes (none, 2x diagonal, 4x square centred, 4x square
  rotated) are accepted (`psgl_context.c:408-430`) [measured].
- **Buffers:** VBOs are implemented, with `glBufferData`, `glBufferSubData` and
  `glMapBuffer` forwarding to the context (`core_gl_ext.c:194-198`) [measured].
- **Instancing primitives exist in GCM but not in PSGL.**
  `cellGcmSetVertexDataArray` takes a per-attribute `frequency`
  (`sdk/include/cell/gcm/gcm_command_c.h:474`), and `cellGcmSetFrequencyDividerOperation`
  exists (`gcm_command_c.h:704`). PSGL does not expose them [measured].

### 1.2 Shader compiler

- **Profiles:** `rsx-cg-compiler` accepts `sce_vp_rsx` and `sce_fp_rsx`, plus the PSP2
  aliases (`tools/rsx-cg-compiler/src/main.cpp:194-200`). The front end is the imported Vita
  Cg front end in `src/cg_frontend`; the NV40 back end is in `src/nv40` [measured].
- **Control flow:** fragment control flow lowers to predicated writes. Forward if/else is
  flattened; `discard` and run-time loops landed in v0.23.0/v0.24.0
  (`docs/design/shader-compiler-control-flow.md:3-5, 45-63`) [measured].
- **Register pressure** is not modelled (`docs/design/shader-compiler-register-pressure.md:18`)
  [measured].
- **GLSL:** `tools/psgl/glsl_subset_to_cg.py` (52 lines) checks that the source contains a
  fixed set of tokens and refuses it if any is missing. It does **not** validate the GLSL. A
  source that contains the tokens plus arbitrary other code is accepted, and the script emits
  the same fixed Cg template regardless. It is a template selector, not a compiler or a
  validator [measured].

## 2. RSX capability (NV40/G70 class)

| Area | Capability | Provenance |
|---|---|---|
| Vertex program size | We refuse more than 512 instructions (`sdk/include/cell/gcm/gcm_cg_bridge.h:243`) and address constants up to 512 vec4 (`tools/rsx-cg-compiler/src/vp_uniform_struct_layout.h:165`). These are our limits, not a hardware measurement. | [measured] |
| VP/FP opcode sets | Opcode encodings in `tools/rsx-cg-compiler/src/nv40/nvfx_shader.h` and `nv30-40_3d.xml.h`. The latter is generated by rules-ng-ng from `nv30-40_3d.xml` (31709 bytes, 2010-09-05) per its header (`nv30-40_3d.xml.h:1-12`). | [in-tree public] |
| Vertex branching, loops, address register; fragment dynamic-branch cost | Commonly stated for the shader-model-3 class. | [unverified] |
| Vertex texture fetch | Commonly stated: available for a few float formats only. | [unverified] |
| Fragment registers | Fewer live temps mean more parallelism (`docs/design/shader-compiler-register-pressure.md:22-25`). | [measured] (our analysis) |
| Integers | No native integer ALU in the shader instruction set we encode (`nvfx_shader.h` lists float opcodes only). Integer semantics need emulation. | [in-tree public] for the opcode list; the emulation cost is [estimate] |
| Texture formats | `CELL_GCM_TEXTURE_*` includes B8, A4R4G4B4, R5G6B5, A8R8G8B8, G8B8, D1R5G5B5, DXT1/23/45, DEPTH24_D8(_FLOAT), DEPTH16(_FLOAT), W16Z16Y16X16 float, W32Z32Y32X32 float and X32 float (`sdk/include/cell/gcm/gcm_enum.h:259-281`). The float filtering limits are [unverified]. | [measured] |
| Render targets | Up to 4 colour targets (MRT1-3, `gcm_enum.h:200-202`); MSAA modes as in §1.1. | [measured] |
| Instancing | Per-attribute frequency and a frequency divider operation exist (`gcm_command_c.h:474, 704`). Full GL instancing semantics (`gl_InstanceID`, a per-attribute divisor) would need to be built on these. | [measured] |
| Geometry, tessellation and compute stages; transform feedback | Not natively supported: there is no such stage or command in the GCM headers we ship. No practical conformant implementation has been established in this design. CPU or SPU emulation is not ruled out. | [measured] absence in `gcm_enum.h` / `gcm_command_c.h` |

## 3. Tier mapping

Effort scale: S = days, M = 1-3 weeks, L = more than a month [estimate].

### 3.1 OpenGL ES 2.0 + GLSL ES 1.00

| Feature | RSX | Ours today | Effort |
|---|---|---|---|
| Programmable VS/FS, attributes, uniforms, varyings | feasible | Cg binaries only | M (GLSL front end) |
| GLSL ES 1.00 language | feasible with emulation: `int`/`ivec` may be represented as floats ([GLSL-ES-1.00] §4.1.3, §4.5), but integer semantics and the precision rules still apply and must be emulated and tested | none (template selector) | M-L |
| On-target `glCompileShader` | feasible but large (ship the compiler on the PPU) | stub | L, optional |
| Minimum limits: `MAX_TEXTURE_IMAGE_UNITS` and `MAX_COMBINED_TEXTURE_IMAGE_UNITS` at least 8, `MAX_VERTEX_ATTRIBS` at least 8 ([ES2] Table 6.20; [GLSL-ES-1.00] §7.4) | to be established | 4 units (`psgl_context.h:11`), 4 attributes (`psgl_context.h:12`) | M (audit and raise) |
| FBO with texture and renderbuffer attachments | feasible | texture only | S-M |
| `glGenerateMipmap`, NPOT textures with clamp | feasible | no-op | S |
| `glReadPixels` (ES2 core) | feasible | no-op (`core_gl.c:497`) | S |
| LUMINANCE, ALPHA, LUMINANCE_ALPHA upload | feasible | not accepted | S |
| RGB/565/4444/5551/8888 upload | feasible | implemented (converted to A8R8G8B8) | — (optional: native-format storage, S) |
| Cube maps | feasible | 2D only | S-M |
| Depth/stencil, blending, scissor, viewport | feasible | implemented | — (semantics audit, below) |

**ES2 conformance audit, a required part of the plan.** Adding a `GLES2/gl2.h` and forwarding
entry points does **not** make an implementation conformant. The audit covers:
- **Limits:** every implementation limit in [ES2] Table 6.20. The two known gaps are
  `PSGL_MAX_TEXTURE_UNITS 4` and `PSGL_MAX_VERTEX_ATTRIBS 4` (`psgl_context.h:11-12`).
- **Query results:** `glGet*` for every ES2 state. Today `GL_NUM_COMPRESSED_TEXTURE_FORMATS`
  is 0 while DXT is accepted (`core_gl.c:74`).
- **Errors:** the GL error required for each invalid call.
- **The binary-only shader contract** ([ES2] §2.10). If there is no on-target compiler:
  - `GL_SHADER_COMPILER` must report `GL_FALSE`;
  - `GL_NUM_SHADER_BINARY_FORMATS` and `GL_SHADER_BINARY_FORMATS` must list the CGB format;
  - `glShaderSource`, `glCompileShader` and the related calls must generate the specified
    `GL_INVALID_OPERATION`.

  Today `glCompileShader` sets a log and returns no error (`gl_shader.c:276-280`). An
  on-target compiler can stay optional.

### 3.2 OpenGL ES 3.0 / GLSL ES 3.00

| Feature | RSX | Ours | Effort |
|---|---|---|---|
| Integer types and bit operations (`uint`, `<<`, `&`, `%`) with ES 3.00 exactness | not natively supported. Multi-component float emulation or CPU paths are not ruled out, but no practical conformant implementation has been established. | none | L or out of scope |
| Uniform buffer objects | emulable (copy to constants) | none | M |
| MRT (`gl_FragData`/`layout(location)`) | feasible, 4 targets | none in PSGL | S-M |
| 3D textures, 2D array textures | 3D feasible; arrays emulable (3D/atlas) | 3D no-op | M |
| Instancing (`gl_InstanceID`, divisor) | build on GCM frequency/divider (`gcm_command_c.h:474, 704`) plus a compiler built-in | none | M |
| Transform feedback | not natively supported; no practical conformant implementation established | — | out of scope |
| Occlusion queries | feasible (GCM reports) | no-op | S |
| sRGB textures/targets | partial | none | M |

### 3.3 Desktop GL 2.1 / GLSL 1.20

This is close to the hardware's native generation. It adds over ES 2.0:
- **Display lists and immediate mode:** emulable on the CPU.
- **Full fixed-function:** including the texenv combiners (today a no-op), texgen and two-sided
  lighting.
- **Raster state:** `glPolygonMode`.
- **Formats and queries:** 3D textures, point sprites, occlusion queries.

All are feasible, at S-M each [estimate].

### 3.4 GL 3.3 core / GLSL 3.30

GL 3.3 core needs integer shader semantics, texture buffers, geometry shaders and transform
feedback, none of which the RSX supports natively. **Out of scope as an engineering
recommendation**: the cost of emulating them is not justified, and partial emulation would
mislead users about what runs. This is not a proof that it is impossible.

## 4. Architecture options

1. **A GLSL front end inside `rsx-cg-compiler`** (agreed). Parse GLSL ES 1.00, and later the
   ES 3.00 subset, into the same IR the Cg front end in `src/cg_frontend` produces. The NV40
   back end, the optimizer, the tests and the CGB container output are then shared.
   - Cg and GLSL differ mainly in front-end semantics: qualifiers, built-ins, implicit
     conversions, and the integer and precision rules.
   - Integer behaviour lowered to float needs its own tests.
   - This replaces `glsl_subset_to_cg.py`.
2. **`--psgl-ffp-library`** (agreed). The fixed-function generator moves into the compiler, so
   one tool builds `shaders.bin`. Extending the state mask to texenv and texgen is the
   prerequisite for desktop GL 2.1 fixed-function.
3. **Runtime API layering.**
   - **(a)** Grow libPSGL into an ES 2.0 superset of its ES 1.1 API, as PSGL already mixes both.
   - **(b)** Write a separate `libGLESv2` over GCM.

   Recommendation: (a). It keeps one context and state model and one set of samples, and
   avoids two drivers fighting over GCM. `GLES2/gl2.h` would be added only together with the
   §3.1 audit.
4. **Test strategy.**
   - **Compiler:** the rsx-cg suite plus golden IR/ucode for each GLSL feature, including the
     integer and precision cases.
   - **Pixels:** the shader differential rig compares rendered output for Cg vs GLSL versions
     of the same shader.
   - **Runtime:** a conformance subset of limits, queries and errors as RPCS3 regression rows.
   - **Hardware:** a hardware pass through ps3dk-run protocol 2 for anything RPCS3 is known to
     get wrong. See the peer-local-store DMA result of 2026-10-25: emulator results are not
     the hardware.

## 5. Recommendation

Target **OpenGL ES 2.0 + GLSL ES 1.00 first**, including the §3.1 limits/query/error audit.
Then add the parts of ES 3.0 the RSX can do (MRT, 3D textures, occlusion queries, UBOs and
instancing over the GCM frequency divider) as PSGL extensions, without claiming ES 3.0. Keep
GL 3.3 out of scope. Phases:

1. **Next minor:**
   - `--psgl-ffp-library`, keeping the builder as a wrapper for one release;
   - fix the cheap ES 2.0 gaps: renderbuffers, `glGenerateMipmap`, `glReadPixels`,
     LUMINANCE/ALPHA uploads, cube maps;
   - report `GL_NUM_COMPRESSED_TEXTURE_FORMATS` correctly;
   - correct the stale "stub" comments.
2. **Following minor:**
   - the GLSL ES 1.00 front end in `rsx-cg-compiler`, with full built-ins and tested integer
     emulation;
   - raise the texture-unit and vertex-attribute limits to the ES2 minimums;
   - the binary-only shader contract (§3.1);
   - `GLES2/gl2.h` over libPSGL, gated on the audit and the conformance-subset RPCS3 rows.
3. **Later:**
   - fixed-function texenv/texgen in the state mask;
   - MRT, 3D textures, queries and instancing as extensions;
   - optional on-target compilation.

Decisions needed from the director:
1. Is the target ES 2.0 (recommended), or desktop GL 2.1 fixed-function as well?
2. Do we ship an on-target GLSL compiler, or keep precompiled binaries only? If precompiled
   only, we must implement the [ES2] §2.10 binary-only contract.
3. Do we extend libPSGL (recommended) or add a separate `libGLESv2`?
4. May ES 3.0-style features ship as extensions under ES 2.0, with no ES 3.0 claim?

## Appendix A: how the entry points were counted

Run from `sdk/libPSGL/src` with `python -I -B`, on `main` e791a391:

```python
import re
for fn in ['core_gl.c', 'core_gl_ext.c', 'gl_shader.c']:
    s = open(fn, encoding='utf-8', errors='replace').read()
    funcs = list(re.finditer(r'GLAPI\s+[^;{]*?\b(gl[A-Z]\w*)\s*\(([^)]*)\)\s*\{', s))
    noop, nontrivial = [], 0
    for m in funcs:
        i, d = m.end(), 1
        while d and i < len(s):            # find the matching closing brace
            d += (s[i] == '{') - (s[i] == '}'); i += 1
        body = s[m.end():i - 1]
        b = re.sub(r'\(void\)\s*\w+\s*;', '', body)        # drop (void)param; casts
        b = re.sub(r'/\*.*?\*/', '', b, flags=re.S).strip()
        if re.search(r'psgl_|g_\w+|return\s+[a-z_]+\(|cg|gl[A-Z]\w*\(|switch|for|while', b):
            nontrivial += 1                 # forwards, writes state, or has logic
        else:
            noop.append(m.group(1))
    print(fn, len(funcs), nontrivial, len(noop), ' '.join(noop))
```

A body counts as "forwards / nontrivial" if it calls a `psgl_*`, `cg*` or `gl*` function,
writes a `g_*` global, returns a function call, or contains `switch`/`for`/`while`. Anything
else, such as only `(void)` casts or constant returns, counts as a no-op. This is a heuristic.
It is not a semantic review.
