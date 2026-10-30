# Scalar cube textures and mixed sampler validation

Independent literal pixel expectations cover six face axes and corners, retained
face updates, completeness and logical-unit routing. Indexed draws always use
an EBO. Both client vertex arrays and VBOs run the conflict/recovery pairs;
changing only sampler selections must recover the same geometry.

The fixture targets the documented level-zero, uncompressed linear-color subset.
It does not establish PS3 hardware support. That gate belongs to sample card
t_d7664220 and remains open until a physical-console run.

Build with the normal PPU toolchain; targets cover ILP32 and LP64. Optional
PSGL_ARCHIVE_ilp32 / PSGL_ARCHIVE_lp64 select private libraries for exact-base
RED and candidate comparisons. The same fixture and shader bytes run on both.
