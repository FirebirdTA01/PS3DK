# PPU comparison carry witness

This draft measures optimized equality/inequality operations against literal
results in a separate O0 judge. It does not change the compiler or use the
volatile workaround in any function under test. No LTO is permitted.

Six executables cover ILP32/LP64 at O1/O2/O3. Each must execute exactly 115
checks: four type/constant families, seven comparison expressions and four
data sets, followed by three register-operand controls. Empty input is included.
Configure with the ILP32 toolchain; CMake refuses an LP64 base configuration.
Both translation units assert their target's pointer, long and int sizes.
Packaged outputs go into the private build's `packaged` directory; an in-source
build is refused. Retain the emitted ABI-width line in each guest log.

Candidate acceptance requires process exit 0, exactly one summary with 115
checks and zero failures, the OK marker, and no mismatch lines. Parent RED
requires retained numbered mismatch records and exit 1, not a build, loader,
timeout or setup failure. The exact parent failing rows must be measured before
freezing the parent judge. Missing or duplicate summaries are invalid.

Retain compiler and runtime hashes, compile/link commands, ELF/SELF hashes,
disassembly (and RTL for the minimal reproduction), guest logs and exit status.
The 64-bit family controls source semantics but returns an int count; disassembly
must establish which backend patterns it actually exercises. Passing host builds
would validate the fixture only, not PPU code generation.

This row is not yet added to the release regression manifest. Source is pending
independent review and the initial measured parent gate.
