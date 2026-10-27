# hello-psgl-multitexture

Fixed-function texturing on texture units 0-3, one texture function per quad.

| Quad | Unit 0 | Unit 1 | Units 2-3 |
|---|---|---|---|
| top left | gradient, `MODULATE` | checker, `MODULATE` | off |
| top middle | gradient, `MODULATE` | checker, `DECAL` (checker alpha mixes it over the gradient) | off |
| top right | gradient, `MODULATE` | checker, `ADD` | off |
| bottom left | gradient, `MODULATE` | checker, `BLEND` toward a magenta env colour | off |
| bottom middle | gradient, `MODULATE` | checker, `REPLACE` | off |
| bottom right | gradient, `MODULATE` | checker, `MODULATE` | stripes `ADD` on unit 2, ramp `BLEND` toward yellow on unit 3 |

Unit 0 alone uses libPSGL's built-in program. Units 1-3 need programs from
a shader library: `state.txt` lists the enabled-unit sets the sample draws
(bit 16+k = unit k), `rsx-cg-compiler --psgl-ffp-library` builds the
library at build time, and the sample loads it with `psglLoadShaderLibrary`.
Without the library, draws that need units 1-3 are refused with
`GL_INVALID_OPERATION`, and the sample reports that and exits nonzero.

The sample prints `hello-psgl-multitexture: ready`, then `done` after 900
frames (or `quit` when Quit is chosen from the XMB).

Requires an SDK newer than v0.26.0: `rsx-cg-compiler --psgl-ffp-library`
and fixed-function texturing on units 1-3 are not in v0.26.0. The next
release ships both.
