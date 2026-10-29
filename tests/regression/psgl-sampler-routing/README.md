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

This fixture covers reflected scalar fragment sampler2D resources in physical
slots 0-7. Sampler arrays and glUniform1iv are covered by psgl-sampler-arrays;
named-program glGetUniformiv queries are also covered by psgl-uniform-query.
Cube/vertex texture support and mixed-type alias validation remain tracked
gaps. Unit values below zero or at least eight are rejected with
INVALID_VALUE before mutation as an implementation guard. No general
ES 2.0 conformance or hardware acceptance is claimed.

The map is copied from program state into the current context on bind.
Texture emission binds each selected logical texture to its binary physical
slot without altering shader instructions or shared Cg parameter values.
A successful relink resets selections; a failed relink preserves the installed
map. Direct Cg fragment binding clears that map until the next glUseProgram.
Unsupported sampler types keep their prior link acceptance and physical
bindings, but glUniform1i on them returns INVALID_OPERATION. Ambiguous
fragment reflection assigning multiple samplers to one physical slot refuses
link, as do malformed sampler2D groups, unsupported resource ranges and
descriptor exhaustion. The cube fixture tests link/bind acceptance and setter refusal, not cube
upload or sampling (the upload path only supports 2D textures). The public
Cg type-class query still lacks sampler classification; this GL path checks
the sampler types explicitly. Existing global GL shader-object bookkeeping
is not changed into a complete per-context implementation by this increment.

See OpenGL ES 2.0.25 section 2.10.4 (uniform/program ownership and samplers):
https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf
