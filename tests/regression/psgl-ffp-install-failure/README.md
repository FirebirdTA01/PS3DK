# Failed fixed-function program installs

The row draws with the built-in unit-0 texturing programs, then switches to
the untextured position-only programs while the RSX local heap is exhausted,
so the first RSX copy of the position-only fragment ucode cannot be made.

That draw must be refused with `GL_OUT_OF_MEMORY` and draw nothing. After the
test frees the heap, the same untextured state must install its programs on
the next draw: the quad takes the current colour, not the texel left by the
texturing programs. Two control draws then switch back to texturing and to
untextured again.

The heap is taken with `rsxMemalign`, largest blocks first, and released
before any readback. Draw rows retry an identical draw a bounded number of
times because RPCS3's asynchronous shader compiler may skip a draw of a
program it has not compiled yet; the final pixel must match.

This is a failure-path regression for the install bookkeeping, not an
allocation-policy test: it does not check how much RSX memory libPSGL needs.
