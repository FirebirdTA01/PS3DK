# XMB → Game Launch — Manual Regression Checklist

Run this checklist by hand after each phase.  
**Expected behavior changes at Phase 7** — before Phase 7, the existing
destructive process-replacement behavior is preserved and any deviation
from it is a regression.

Last verified: (fill in after each check)

---

## Checklist

| # | Step | Expected (Phase 0–6) | Expected (Phase 7+) |
|---|------|----------------------|---------------------|
| 1 | `[ ]` Launch from-source RPCS3 (no CLI arg, GUI starts) | Window opens, no crash | Window opens, no crash |
| 2 | `[ ]` Boot VSH (Tools → Boot VSH, or `Ctrl+N`) | XMB loads, cursor visible | XMB loads, cursor visible |
| 3 | `[ ]` Navigate to Game column in XMB | Games listed, no freeze | Games listed, no freeze |
| 4 | `[ ]` Select installed game (Rebug Toolbox or any HDD game) | Icon / title highlights | Icon / title highlights |
| 5 | `[ ]` Press X to launch game | Fade to black, then game boots | Fade to black, then game boots |
| 6 | `[ ]` Wait for game to be interactive (10–30s) | Game renders, input works | Game renders, input works |
| 7 | `[ ]` Game-side quit (in-game menu → Quit Game, or `ps` button if supported) | Game exits | Game exits |
| 8 | `[ ]` Observe RPCS3 after game exits | **RPCS3 stops / window closes** (destructive replace) | **XMB resumes** (non-destructive, VSH preserved) |
| 9 | `[ ]` Verify no fatal log entries | No `VM: Access violation`, no `PPU: Unregistered function called`, no `TODO` new to this phase | Same |

---

## Phase-specific notes

### Phase 0–6: Existing destructive-exit behaviour
- After game exits, RPCS3 terminates (or returns to idle, depending on config).
  This is the *baseline* — any earlier exit or crash is a regression.
- The `[ ]` items above should all pass if XMB → game launch was working
  before the phase change.

### Phase 7+: Non-destructive VSH resume
- After game exits, the XMB should be visible with the cursor still on the
  game that was just exited (same as real PS3).
- `sys_process_getpid()` in the VSH process should still return 1
  post-resume.
- Game's process should be fully destroyed (no leaked memory containers,
  SPU thread groups, RSX state).

---

## Verification log

| Date | Phase | RPCS3 git rev | Tester | Result | Notes |
|------|-------|---------------|--------|--------|-------|
|      |       |               |        |        |       |

