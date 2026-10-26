# PSGL texture upload storage

This row checks byte `ALPHA`, `LUMINANCE`, `LUMINANCE_ALPHA`, `RGB` and `RGBA`,
plus the two packed RGBA 8888 types, on ILP32 and LP64. The logical channels
expand according to OpenGL ES 2.0 table 3.12: alpha supplies zero RGB,
luminance supplies equal RGB and opaque alpha, and luminance-alpha supplies
equal RGB and the supplied alpha. Storage remains ARGB8.

The public texture-reference buffer path exposes the actual uploaded words
through `glMapBuffer`. Each format uploads a 3-by-2 patch at a nonzero offset,
with unpack alignments 1, 2, 4 and 8. Literal expected texels, untouched
destination words (including pitch padding), and unchanged source bytes are
checked. RGB/RGBA byte uploads are controls for the new formats and packed
row-stride repair. Each format also exercises `glTexImage2D` allocation;
framebuffer completeness is used only to observe allocated storage, not as
a claim about logical-format renderability.

Success requires exactly `PSGL_UPLOAD_OK checks=35 failures=0` and a normal
zero exit. The pre-fix implementation fails 23 checks: 15 for the three
unsupported formats and 8 for the two packed types' second rows. This is a
storage/stride test, not GPU sampling, filtering or ES conformance coverage.

The existing non-render-target `glTexImage2D` path still does not reject
`internalformat != format`. General texture argument/error validation and
logical internal-format conversion rules remain separate audit work; this
test uses matching formats.
