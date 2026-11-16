# B0: completed report to MAIN through the report DMA object

An explicitly authorized hardware experiment, not a production profiler path.
The input object's permissions and offset base are unknown on hardware. This
probe can fault or wedge the RSX. Only the reviewed, sealed launch packet is
eligible for the director-authorized single run; an emulator match proves no
hardware permission or ordering claim.

Derived from `hello-ppu-gcm-report-ring` at b02c582c8, which reached its
addressability refusal cleanly on the PS3. It includes that sample's unchanged
`state_tour_setup.h` and uses the original state-tour shaders. Surfaces, pitches,
formats, geometry, texture, shaders, FIFO and host heap are unchanged. No flip
or zcull binding is issued. The original ordering probe remains untouched.

Intentional deviations:

| Change | Reason |
|---|---|
| Only baseline timestamps at reports516/517 and one baseline draw | Obtain a completed reference; no report reuse or concurrent draws |
| No CPU writes to report space, including sentinels | Director's B0 scope; freshness comes from changed timestamp, ordered pair and final idle |
| Remove poison allocation, banks and 192-trial matrix | B0 tests copying only |
| Read back the whole 1 MiB MAIN arena's guards; use one 16-byte destination | Detect writes outside the authorized destination |
| Direct 12-word M2MF packet, input 0x66626660/output 0xFEED0001 | Ordinary transfer wrapper would overwrite the experimental input binding |
| Try source offsets 0x3440 then 0x2040 for index 516 | Explicitly requested hypotheses about the report-object base |
| Omitted-copy control before candidates | A stale destination must not count as an exact copy |
| B0 log/crumb namespace | Keep outcomes distinct from the ordering experiment |

Every fence label 110..114 is reset to zero and read back. Subsequent targets
increase. The reference command uses a fresh value. MAIN memory is allocated
and mapped once. The fixed white quad, ordered timestamps and ordinary LOCAL
to MAIN copy must pass first. After the report writes complete, its first 16
bytes are the immutable reference. No subsequent report command is issued.

The omitted-copy control resets MAIN to a sentinel, performs a bounded final
fence without a copy, and must mismatch the reference. Each candidate resets
MAIN to a fresh sentinel and makes one 16-byte CPU read of the known report
pointer. It refuses a changed reference. It then emits one 16-byte copy and
performs a bounded final fence before comparing all 16 bytes and every guard.
A first exact match stops. A mismatch proceeds only if the RSX is idle and all
guards are intact. A timeout, corruption or invalid reference ends the run.

RPCS3's current report-handle resolver adds report-array offset 0x1400 itself;
candidate 2 is therefore the expected emulator match. This inference is not a
hardware base claim. The packet records what the selected emulator actually did.

Bounds and recovery are inherited: 120 s cooperative session budget, 130 s absolute
drain budget, 2 s per wait. Deadline checks precede accepting fence completion.
Blocking firmware, flush or file I/O can delay a cooperative check. Normal exit
requires final backend label, fresh reference and GET==PUT. No mapped allocation
is freed. Codes: 0 exact candidate with sensitive control; 2 no match or insensitive
control; 3 invalid baseline/reference/guards or deadline with idle exit.

On uncertain pending work or fence timeout: release all armed PPU-owned gates
before logging, then terminal `B0_RECOVERY_REQUIRED`: no GPU commands, no frees,
no return, no exitspawn, no retry. B0 queues no PPU-owned gate; the preserved
release helper is consequently a no-op. Backend self-waits are not PPU gates.
The hook watchdog at host timeout 240 s may still exitspawn RELOAD.SELF; that can
hang after an RSX fault and is not recovery. The operator reboots if necessary.

An exact B0 copy establishes only those 16 bytes in that quiescent trial. It
does not establish A1 report-before-copy, A2 safe reuse, B2 early publication,
larger blocks, repeated use or other contexts. Those require separate probes.

Host gate: `python3 tests/host/report-copy-b0-test.py`. It checks packet words,
16th-byte mismatches, omitted-copy sensitivity, guard coverage and stop policy,
with five required killed mutants. The original report-ring host gate checks
unchanged setup provenance and bounded recovery. Build with the PPU CMake
toolchain; `-Werror` applies. No new SDK code or wire format is introduced.
