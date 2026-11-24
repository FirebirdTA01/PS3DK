# hello-ppu-prof-pixels

Known answers for the RSX frame capture's passed samples per draw and zcull state (`PS3_PROF_RSX_FRAME`, decoder 5;
profiler guide, "Pixels per draw" and "Zcull per draw"). Every frame draws the same list on the display surface
(W x H, one sample per pixel), with a zcull region bound over the depth buffer at start-up:

| Draw | What | Passed samples |
|---|---|---|
| `clear` | colour, depth and stencil clear | not checked |
| `full` | full-screen quad, depth test off | W x H |
| `half` | left half of the screen, depth test off | W x H / 2 |
| `clear_depth` | depth and stencil clear | not checked |
| `front` | full-screen quad at depth 0.25, LESS, depth writes on; zcull invalidated before it | W x H |
| `behind` | full-screen quad at depth 0.75, behind `front` | 0 |
| `clear_depth2` | depth and stencil clear | not checked |
| `back_first` | full-screen quad at depth 0.75 | W x H |
| `front_second` | full-screen quad at depth 0.25, in front of `back_first` | W x H |
| `kil_half` | full-screen quad whose program discards uv.x < 0.5 | W x H / 2 |
| `app_query` | full-screen quad inside the app's own ZPASS occlusion query (report 600) | W x H |

The app requests three captures itself: frame 30 `pixels` with `PS3DK_PROF_RSX_FRAME_PIXELS` (every quad counted:
forced, and `app_query` counting), frame 60 `observe` without it (only `app_query` counted; the rest are `off`), and
frame 90 `pixels-both` with BOTH | PIXELS (the same counts in the pipelined and the isolated frame, paired per draw).
After each frame it reads its own query's report and prints `PIXELS appquery frame=<n> value=<v> expect=<W x H>` for
the captured frames (and any mismatch): the capture must never change the app's own result. At exit a fourth capture,
`zcull-unbind`, records the zcull region's release (GPU safety below).

## Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=<repo>/cmake/ps3-ppu-toolchain.cmake \
  -DPS3_PROF_AUTO=ON -DPS3_PROF_AUTO_RING=ON -DPS3_PROF_AUTO_RING_MB=1 -DPS3_PROF_AUTO_RING_RECORDS=4096 \
  -DPS3_PROF_RSX_FRAME=ON
cmake --build build
```

The configure step refuses without any of the three options.

## Run and check

The TTY shows `PIXELS start ... width=<W> height=<H>`, the three `PIXELS request frame=<n> result=<id>` lines, the
app-query lines, `PIXELS drain flip=ok fence=ok`, `PIXELS zcull unbind rc=0 capture=<d>` and `PIXELS EXIT clean
frames=120 captures=<a>,<b>,<c>,<d> appquery_mismatches=0 zcull_unbind=0` (exit 0; 2 when a request was refused, the
app's query changed in a captured frame or the unbind failed). Then:

```sh
python3 tools/check_pixels.py --reader ps3dk-prof --capture <executable>-<timebase>.prof --tty TTY.log
```

It prints one `DIFF` line per difference and exits 0 only on a full match. Besides the counts it checks the counting
states, that every quad is timed, that zcull region 0 is recorded as bound before each capture and covers every
quad's depth buffer with depth cull on, that `front` carries the zcull invalidation, and that `kil_half` is marked
early-z off. `host-tests/test_check.py` checks the checker itself without a capture. Clears are not checked: what
the counter does over a clear is not characterized.

## GPU safety

As `samples/profiling/rsx-frame`: the fence label (200) is zeroed and read back first, every wait is bounded and
services system callbacks, a failure stops submitting and goes to the exit drain, and if the drain cannot prove the
RSX idle the sample prints `PIXELS RECOVERY_REQUIRED at=<where>` and parks (no command, no seal, no return). The
sample binds one zcull region over its own depth buffer (allocated at the 64 x 64-aligned size the region covers) and
uses report 600 from the application range only. The zcull binding is driver state that may outlive the process,
so it is released before the seal: after the drain proves the RSX idle, zcull is turned off, a fence is flushed and
waited for, and region 0 is unbound (inside the `zcull-unbind` capture, which records it); a failed unbind fails the
exit (2). The park path sends nothing, so it does not unbind. The bind is the last init step: no init failure follows
it.
