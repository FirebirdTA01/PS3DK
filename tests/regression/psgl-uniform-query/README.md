# Program-owned sampler queries

The row tests scalar fragment sampler2D glGetUniformiv queries on the named
program, including when no program or a different program is current. Values
are logical texture units, not physical TEXUNIT indices. Two programs sharing
one shader have independent selections. Successful relink resets selections;
failed relink makes queries invalid. Output fences and sentinels check errors
leave all output untouched. Cube queries are explicitly unsupported here.

The weak entry-point reference lets the old library execute the same row:
absence fails, and no replacement getter is supplied. Other GL calls force
extraction of gl_shader.o. Both ABIs use the real target archives and API.
The direct-Cg fixture uses the existing archive-visible memory loader because
public cgCreateProgram does not yet accept embedded binaries.

This is a query-state regression, not a render or conformance test. Numeric
uniform getters and glGetUniformfv remain unsupported: numeric parameter
storage is currently shared by programs using one shader object. General
numeric getters need the independent program-state repair first.

Specification basis: OpenGL ES 2.0.25 section 6.1.10, pp. 133-134:
https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf
