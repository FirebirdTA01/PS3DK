# Report-copy ordering probes

Three separate executables (A1, A2, B2), derived from `hello-ppu-gcm-report-copy-b0`
at 05c84fccc. B0 completed on the PS3 on 2026-11-16: report DMA input object
`0x66626660`, source offset `16 * report_index`, ordinary MAIN output object
`0xfeed0001`, exact 16-byte match and intact guards after a full drain. That
establishes this copy route for one completed report, not its ordering edges.
Each executable needs its own reviewed launch packet. Do not run all three
automatically on hardware. RPCS3 results are emulator-only.

## Proven setup and deviations

Include the original report-ring `state_tour_setup.h` unchanged, which preserves
the console-tested state-tour setup: 64x64 local A8R8G8B8/Z24S8 surfaces, 256-byte
pitches, shaders, white texture, quad/vertex bindings, FIFO and 8 MiB host heap.
Preserve B0 label reset/readback, bounded waits, reference/GET==PUT final drain,
memory barriers, mapped allocation lifetime, crumb output and terminal recovery.
No flips, zcull bindings, main-location timestamps or CPU report-memory writes.

All deviations from B0:

| Change | Purpose |
| --- | --- |
| Separate compile-time A1/A2/B2 targets | One claim and one packet per launch |
| Fixed source `16*index`; no candidate search or local-address conversion | Use the route B0 established on hardware |
| Variable M2MF line length 32/224/4032 bytes | Test 1/7/126 timestamp pairs, including a large block |
| Two alternating banks at reports 516..767 and 768..1019 | Reuse generations without leaving the application range |
| Save report guards 512..515 and 1020..1023 before each trial | Detect unintended report writes without writing report sentinels |
| Seed every active report with a completed RSX timestamp/draw generation | Known prior contents/freshness; no CPU writes into firmware mapping |
| Two MAIN destinations at 128 and 8192 in the same 1 MiB arena | Preserve old/new copied generations simultaneously |
| New sentinel per trial, check every MAIN word outside active destinations | Detect truncation, wrong destination and overrun |
| Backend label/wait barriers and B2 command-label/PPU wait control | Measure the two lifetime edges and publication separately |
| Eight candidate and eight negative trials per length | Workload-specific observations with sensitive controls |

Each length first performs a quiescent copy of its entire block and requires an
exact match, valid timestamp pairs and intact guards. A failure stops that
packet; larger copy sizes are not assumed proven by the original 16-byte B0.
Baseline remains white pixel, changed ordered timestamps, ordinary LOCAL-to-MAIN
copy and caught omitted-copy control. Source report mapping must be contiguous.

## Experiments

Every trial starts idle, seeds a completed generation and freezes its bytes.
The CPU never modifies reports. Timestamps must be nonzero, ordered, and a new
generation must start strictly after the prior generation ends.

* **A1 report-before-read:** new reports, backend label + GPU wait, report copy.
  Compare the copy with the completed new generation. Control: copy the old
  generation first, backend label + wait, then write the new generation; the
  comparison must detect the old bytes. The control tests detector sensitivity;
  it does not establish that an omitted barrier naturally fails on every GPU.
* **A2 read-before-reuse:** copy old generation, backend label + wait, rewrite
  reports, backend label + wait, copy new generation to the second destination.
  Both copies must match their own generation. Control: overwrite before copying
  old, so the old-generation comparison must fail. The new copy must still match.
* **B2 publication:** stable completed source, copy, backend publication + wait.
  PPU freezes the destination immediately when it sees that publication, before
  waiting for final idle. Compare the frozen bytes, never substitute a later
  snapshot. Control: command publication then a PPU-owned RSX wait before the
  copy; the frozen destination must still be sentinel. Release the gate, drain,
  and require the settled copy to match. Later correctness cannot repair early
  publication evidence.

For each length, all eight valid candidates and at least one caught control are
required for `SUPPORTED_THIS_WORKLOAD`. Any valid candidate mismatch is
`REFUTED`; missing trials or no caught control is `INCONCLUSIVE`. Invalid
reference/generation/guards stop with `INVALID`. No finite passing count proves
universal ordering, CALL replay safety, SPU writer safety or another memory
mapping. A1/A2/B2 remain separate premises, not interchangeable fence proofs.

## Bounds and recovery

Same B0 limits: 120 s cooperative session, absolute 130 s final drain, 2 s per
wait. Deadline wins over a simultaneously observed completion. Firmware calls,
flush and logging may delay checks; file I/O is not hard-bounded. Labels110..114
are reset/read back at startup and subsequent targets increase strictly.

The B2 control arms label113's highest target **before** emitting its wait.
Every abort/recovery path first releases that target by a PPU store and sync,
before logging or draining. Repeating the last release is safe. It does not
repair a DMA fault or an RSX hang. Backend self-wait labels are never released
by fabricated PPU stores.

Normal return requires final label, reference and GET==PUT; mapped buffers are
never freed. On uncertain pending state or a fence/publication timeout, release
PPU-owned gates and enter terminal `ORD_RECOVERY_REQUIRED`: no further GPU
commands, free, return, exitspawn or retry. Operator recovery only. The existing
run-hook watchdog at host `--timeout 240` can override this policy by attempting
to relaunch the loader; that is not GPU recovery and can hang after a wedge.

Exit 0: all lengths supported for this workload and idle. Exit1: refuted and
idle; exit2: inconclusive and idle; exit3: invalid/setup/deadline and idle.
Missing `ORD_END idle=1` is never a passing run. Crumbs are separate per claim:
`/dev_hdd0/tmp/report-copy-A1.crumb`, `-A2.crumb`, `-B2.crumb`, session-delimited.

Host gate: `python3 tests/host/report-copy-ordering-test.py`; it checks exact
packet bytes, all schedules, frozen-vs-settled evidence, guards, verdicts and
mutants, plus inherited abort functions. Both PPU ABIs are build targets.
