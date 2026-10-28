# Voice, debug-font, search and SAIL record layout check

Run `bash tests/sdk/media-record-layout-test.sh` with PS3DEV set to a built
PPU toolchain. `--require-toolchain` makes an absent toolchain a failure;
otherwise the script reports an explicit SKIP. Nothing is installed.

The four groups compile in GNU C99, C11 and C++17 under ILP32 and LP64.
They assert literal sizes, alignment, member widths and member offsets,
and emit actual objects. Five isolated header mutations must each fail
with the relevant assertion in both data models. These are compile tests,
not observations of voice, search or SAIL services on a console.

Voice port records retain 64-byte alignment without rounding their size to
64 bytes. Do not assume sizeof is an array stride for these over-aligned
typedefs; use separately aligned objects or an explicit padded wrapper.
Search timestamps remain 64-bit regardless of the C library time_t width.
CellSailStartCommand and CellSailSourceStartCommand are distinct records;
the source-callback command remains unchanged.

The debug-font GCM configuration and console configuration have changed
layout. Rebuild libdbgfont together with its callers; mixing new callers
with an older installed archive is unsupported. The compatibility wrapper
also preserves the complete 32-bit screen dimensions.
