# Eight fragment texture units

This row binds distinct one-pixel textures to units 0-7, samples each slot
independently, then samples all eight in one binary program. It also checks
high-unit rebinding, deletion of a texture shared by all units, invalid
active-unit selectors preserving state, and the fragment/combined/vertex
limit queries through integer, float and boolean entry points.

The binaries have explicit TEXUNIT resources. This does not validate
glUniform1i sampler remapping, which is a separate implementation gap.
The legacy GL_MAX_TEXTURE_UNITS query remains 4; fixed-function texture
environment stages beyond unit 0 still need implementation for the desktop
GL 2.1 direction. Vertex texture fetching is not implemented (limit 0).

Exact final RGBA bytes are required after bounded asynchronous shader
warm-up. Each draw clears first and expects nonblack output. The test
requires the working ReadPixels implementation and is run in both ABIs
against private archives without replacing the installed SDK. No hardware
or general ES 2.0 conformance acceptance is implied.
