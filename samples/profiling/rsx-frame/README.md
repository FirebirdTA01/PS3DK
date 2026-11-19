# rsx-frame

A known draw matrix for the RSX frame capture (`PS3_PROF_RSX_FRAME`, guide 4.12 and 5.7). Every frame submits the
same 19 draws, so whichever frame is captured, `ps3dk-prof rsx-frame` must read back exactly the list in
`expected/draws.tsv`:

| Draws | What | Label | Tag | AUTO range |
|---|---|---|---|---|
| 0 | clear (colour, depth, stencil) | `frame/clear` | 0x10 | - |
| 1..6 | 2 vertex programs x 3 fragment programs, one quad each; textures A8R8G8B8, R5G6B5 and B8; depth test, back-face cull and alpha blend in varying combinations | `frame/matrix/vp<v>_fp<f>` | 0x100..0x105 | `matrix` |
| 7 | an indexed quad (6 u16 indices in local memory) | `frame/indexed/quad` | 0x200 | - |
| 8..15 | an instance run: 8 identical draws, only a vertex-program constant differs | `frame/instances/run` | 0x300 | `instances` |
| 16 | an inline-array triangle (the vertex data in the command buffer) | `frame/special/inline_array` | 0x400 | - |
| 17 | an inline-index draw (6 u16 indices in the command buffer) | `frame/special/inline_index` | 0x400 | - |
| 18 | an immediate triangle (SetDrawBegin, per-vertex attribute writes, SetDrawEnd; the arrays disabled first) | `frame/special/immediate` | 0x400 | - |

The app requests the capture itself at frame 30 (`PS3DK_PROF_RSX_FRAME_CAPTURE("matrix")`), so no PC app is needed.
At frame 60 it requests a second one, `matrix-both`, with `PS3DK_PROF_RSX_FRAME_BOTH | PS3DK_PROF_RSX_FRAME_FP_BYTES`:
the same draws pipelined, then isolated, each view checked against the same table, the two paired, every program's
bytes stored (decoder 4). With the PC app connected (live auto-start is on), its Capture RSX frame button requests more. After 120 frames the
sample drains the RSX, seals the capture and returns 0 (2 if the request was refused, 1 after a failure).

Textures and fixed-function state vary across the matrix: `expected/state.tsv` lists, per draw, the render state,
texture slot 0, the three vertex attributes, the index stream and the surfaces the capture must show (decoder 2);
fields that depend on the display or on memory placement are left out. `expected/constants.tsv` lists each draw's
VP uniform values (`offset`, `xform`), and the draw table's `@ instance_runs` the one instance run (draws 8..15,
differing in `offset`) (decoder 3).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake \
  -DPS3_PROF_AUTO=ON -DPS3_PROF_AUTO_RING=ON -DPS3_PROF_AUTO_RING_MB=1 -DPS3_PROF_AUTO_RING_RECORDS=4096 \
  -DPS3_PROF_RSX_FRAME=ON -DPS3_PROF_LIVE_AUTOSTART=ON
cmake --build build
```

The configure step refuses without any of the four options.

## Run and check

The TTY shows `RSXFRAME start ...`, `RSXFRAME request frame=30 result=<id>`, `RSXFRAME request frame=60 both result=<id>`,
`RSXFRAME drain flip=ok fence=ok` and `RSXFRAME EXIT clean frames=120 capture=<id> both=<id>`. Then, with the capture copied off the console:

```sh
python3 tools/check_capture.py --reader ps3dk-prof --capture <executable>-<timebase>.prof --shaders build/shaders
```

It prints one `DIFF` line per field that differs (draw table and state table; `--state -` skips the state) and
exits 0 only on a full match. The programs are named by shader
file stem; the checker maps each to its canonical hash with `ps3dk-prof shader-hash` on the built `.vpo`/`.fpo`. Every
draw must also be hooked and decoded, with no mismatch and no unknown state, and every program exact (readable,
terminated, not bound before the capture). `host-tests/test_check.py` checks the checker itself without a capture.

## GPU safety

- The fence label (200) is zeroed and read back before any GPU work: a value left by an earlier app cannot satisfy a
  wait.
- Every flip and fence wait is bounded (2 s each, 60 s for the workload, 10 s for each exit-drain wait) and services
  system callbacks; an exit request stops the workload.
- A refused flip, an expired wait or the session deadline stops submitting at once (`RSXFRAME FAIL at=<where>`),
  nothing is retried, and the exit drain runs.
- The exit drain waits for the last flip and a fresh fence. Only then is the capture sealed and `main` returns. If
  the RSX is not proven idle the sample prints `RSXFRAME RECOVERY_REQUIRED at=<where>` and parks: no GPU command, no
  seal, no return.
