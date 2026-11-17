# hello-ppu-prof-auto-verdicts

Known-answer phases for the automatic RSX profiler's per-frame verdicts. The whole run is one capture window
(`PS3DK_PROF_CAPTURE_WHOLE_RUN`); each phase prints `AUTOVERDICT phase <A..E> frame=<id>` at its first frame.

| Phase | Frames | Workload | Reader verdict |
|---|---|---|---|
| A | 0..59 | heavy fragment work behind `cellGcmSetWaitFlip`, then `cellGcmWaitFlipComplete` | RSX-bound candidate (busy flip wait) |
| B | 60..119 | trivial RSX work, a 45 ms PPU spin, HSYNC flips | UNKNOWN: RSX not limiting; PPU work vs presentation pacing not distinguished |
| C | 120..179 | trivial RSX work, VSYNC flip wait | pacing |
| D | 180..239 | heavy work, polling `cellGcmGetFlipStatus` | RSX-bound candidate (busy polling, flip status) |
| E | 240..299 | heavy work, polling `cellGcmIsLabelPassed` on label 200 | RSX-bound candidate (busy polling, a label) |

The reader emits no PPU-bound verdict without positive PPU-busy evidence, which this capture does not carry, so
phase B's answer is UNKNOWN. A frame's draws run on the RSX while a later frame waits for the flip; phase A reads
RSX-bound with a reader that counts the owner's RSX work from any frame against the wait. The first frame of each
phase can carry the previous phase's verdict, and the first two frames of phase A have no earlier work to wait on.

## Bounds and recovery

- Every flip-status, label and fence wait is bounded: 2 s per wait, 120 s for the whole workload, 130 s for the
  exit drain. The polls service system callbacks; a quit request stops the workload.
- A refused flip submission, a failed `cellGcmWaitFlipComplete`, an expired wait or the session deadline prints
  `AUTOVERDICT FAIL at=<where>` and stops the workload at once: no further frame is submitted and nothing is
  retried.
- The exit drain then waits for the last flip and a fresh label fence. Only when both complete does the sample seal
  the capture (`PS3DK_PROF_AUTO_SEAL`) and return: 0 with `AUTOVERDICT EXIT clean` (or `quit`), 1 with
  `AUTOVERDICT EXIT failed at=<where>`.
- If the drain does not prove the RSX idle, the sample prints `AUTOVERDICT RECOVERY_REQUIRED at=<where>` and parks:
  no GPU command, no seal, no free, no return and no exitspawn. An injected run-hook watchdog can still override
  this at its own timeout and exitspawn RELOAD.SELF; after an RSX fault that can hang and is not recovery, so the
  operator reboots if necessary.

The bounds and the stop and exit policy live in `source/verdicts_logic.h`; `tests/host/prof-auto-verdicts-test.py`
drives them with a fake clock and checks that deliberately broken policies are caught.
