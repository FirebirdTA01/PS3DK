# Vectorizer realignment witness (GCC patch 0082)

`functions.c` is built at -O3, where GCC 16 vectorises the outer loop with a
vectorisation factor of 1: an SLP store group of 16 lanes fed by a splat of
`rows[y]`.  Without patch 0082 the load of `rows[y]` used the optimized
explicit realignment scheme (one `lvsr` mask for `&rows`, then `vperm` of two
`lvx` loads), although the load advances 4 bytes per iteration, not a whole
vector.  Rows 1 and 2 then come from the zeroed stack slot after the table.
The judge in `main.c` is built at -O0 and compares all 64 words with a literal
table, in both data models.

Pass: exit 0, `VECT_REALIGN checks=64 failures=0` and `VECT_REALIGN_OK`.
The v0.26.0 compiler fails both rows with 32 failures (`rows 13ff0000 00000000
00000000 13ffff00`) under the PPU LLVM recompiler and the PPU interpreter.
A Linux-hosted cc1 with patch 0082 (`functions.c` compiled to assembly, then
linked with the Windows SDK) passes both rows under both PPU decoders; the
same cc1 without 0082 fails them exactly as v0.26.0 does.
