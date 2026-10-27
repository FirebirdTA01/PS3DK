# Required-artifact PR guard

`bash tests/sdk/required-artifacts-test.sh` runs the real
`scripts/gen-required-artifacts.sh --check` on fixture repositories and on a
copy of every repository sample `CMakeLists.txt`. No SDK build is required.
Use `--output <new-directory>` to retain inputs, commands and diagnostics.
From native Windows Python, pass `--bash "C:/Program Files/Git/usr/bin/bash.exe"`;
the runner puts that directory ahead of Windows utilities in its child PATH.

The repository check creates empty archive placeholders from the committed
artifact manifest, including alias paths. It separately supplies newlib's
`libm.a` in the toolchain sysroot location. That explicit exception is outside
the SDK manifest and outside `ppu/lib`, so it does not invent a required row.
The expected install is never generated from the sample scanner's output.
A new literal sample dependency must therefore be promised by the manifest
(or separately justified as a toolchain dependency) before the PR can pass.

This validates declared dependency coverage, not whether release archives
were actually built or contain valid symbols. Packaging still runs the same
check against the real install and validates its other required artifacts.

Fixtures exercise quoted and unquoted CMake variables, quoted literal library
names, generated sample-local stubs, no generated stubs, a forced xargs batch
with no stub declaration, a missing literal archive, an uncovered archive,
and the zero-PPU/zero-SPU output floors. Negative cases require both the exact
exit status and their expected diagnostic; a crash cannot count as success.
