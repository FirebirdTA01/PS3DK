# PSGL fragment-uniform ordering

Fragment-program constants live inside the RSX fragment ucode, so changing a
fragment uniform means writing new ucode bytes. This row checks that a draw
keeps the uniform value that was current when it was issued, even while
earlier draws using the same program are still queued.

- A serialized control reads back after every draw.
- Two-draw rounds and a four-draw run change `solidColor` between queued
  draws with no readback between them. Every rectangle must show its own
  colour.
- Dirty-only: redrawing without a change, or setting the value the program
  already holds, must not take a new ucode copy. A value change takes exactly
  one. The copy and ring-wait counters are libPSGL internals, read through
  weak references, so the RED build still links.
- Exhaustion: 6400 queued 2x2 cells each set their own value in one frame,
  which is more changes than the copy ring holds. One readback then checks
  every cell. The ring must wrap at least once, and every cell must keep its
  value.

A bounded warm-up (at most ten attempts, 200 ms apart, count printed) absorbs
RPCS3 asynchronous shader compilation skipping the first cold draw. Nothing
after the warm-up retries. Expected colours use only 0 and 1 channels.
