# Fragment sampler routing

The witness uses binary physical texture slots 0, 3 and 5 and chooses
logical units through glUniform1i. It checks default zero, per-program
retention even with shared shader objects, multiple samplers selecting one
logical unit, invalid setters, link transitions and direct-Cg binding.
An unbound selected texture is expected to sample black with alpha one.
Exact pixels are required after bounded async shader warm-up; every draw
clears to a distinct color first. Both ABIs run against private archives.

The direct-Cg fixture uses the internal binary-memory loader (public
cgCreateProgram does not yet implement binary input). The installed
companion SDK includes a GCM Phase B overlay, not pristine v0.26.0.

The first implementation covers reflected scalar fragment sampler2D
resources in physical slots 0-7. Sampler arrays, glUniform1iv, glGetUniformiv,
cube/vertex texture support and mixed-type alias validation remain tracked
gaps. Unit values below zero or at least eight are rejected with
INVALID_VALUE before mutation as an implementation guard. No general
ES 2.0 conformance or hardware acceptance is claimed.

See OpenGL ES 2.0.25 section 2.10.4 (uniform/program ownership and samplers):
https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf
