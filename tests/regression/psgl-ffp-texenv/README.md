# PSGL fixed-function texture environments

Fixed-function texturing on units 0-3, per OpenGL 2.1:

- Texture environments: section 3.8.13, tables 3.21 and 3.22.
- Per-unit texcoord arrays: section 2.8.
- Current texcoords: section 2.7.
- Texture matrices: section 2.11.2.

Every expected pixel is derived by hand from the tables, and the comments
next to each row show the derivation. Inputs are 0x00, 0xff or a value that
passes through unchanged, so no row depends on rounding. The colour buffer is
XRGB, so alpha results are observed through `SRC_ALPHA` blending over a blue
clear.

## Rows

- MODULATE, including the initial mode with no `glTexEnv` call.
- REPLACE, ADD (including clamping), DECAL (texture alpha 1 and 0) and BLEND
  with an env colour, each on its own unit.
- A four-stage chain, and an enabled-unit gap {0,2} with textures bound on
  the disabled units.
- ALPHA and LUMINANCE base formats.
- A texture matrix on unit 2, which must not affect unit 0.
- A colour array as the primary colour.
- `glMultiTexCoord4f` for a unit without an array.
- DECAL with a fractional texture alpha, and LUMINANCE_ALPHA alpha in
  REPLACE and MODULATE.
- Binding-only changes of the enabled-unit set: unit 1 switched between a
  stored texture and 0, completed by `glTexImage2D`, and left without a
  texture by deletion, with no other call before the draw.
  Unit 0 alone also switches between {0} and the untextured set {}.
- Env colour changed between back-to-back queued draws. Each draw keeps the
  colour set before it.
- Error and state rows: COMBINE, unknown modes, targets and pnames give
  `INVALID_ENUM` without changing state, and `glGetTexEnv{iv,fv}` round-trip
  per-unit state.
- Units 4-7 are texture image units without texture coordinate sets
  (section 2.11.2). `glEnable(GL_TEXTURE_2D)`, `glBindTexture` and `glTexEnv`
  on them store state with no error and `glGetTexEnv` round-trips it, but
  fixed-function draws read units 0-3 only: an enabled unit 4 changes neither
  a textured nor a lit draw, nor the installed programs. Texture-matrix
  operations there are `INVALID_OPERATION`. `GL_MAX_TEXTURE_UNITS` and
  `GL_MAX_TEXTURE_COORDS` are 4, and `glMultiTexCoord` names its own
  coordinate set whatever unit is active.

`psgl-ffp-texenv-nolib-*` runs without a shader library. Unit 0 uses the
built-in program. A state that needs units 1-3 is refused with
`INVALID_OPERATION` and draws nothing, and a unit-0 draw right after such a refused draw is still
exact.

The library is built at configure time from `state.txt` with
`rsx-cg-compiler --psgl-ffp-library`, or from `PSGL_FFP_STATE_FILE`. A
bounded warm-up per row (at most ten attempts, 200 ms apart, count printed)
absorbs RPCS3 asynchronous shader compilation skipping a cold draw. The final
pixel must match exactly.

## Limits

- COMBINE, texgen and point-sprite coordinate replacement are not
  implemented.
- Fog together with texturing has no fixed-function program yet.
- INTENSITY is not an uploadable format here.
- DECAL on ALPHA, LUMINANCE and LUMINANCE_ALPHA textures is undefined in GL.
  It passes the incoming colour through.
