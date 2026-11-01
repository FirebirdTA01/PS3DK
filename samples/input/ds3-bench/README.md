# DualShock 3 buttons, pressure and rumble bench

This C++17 sample selects a connected controller, turns on pressure-sensitive
buttons where the port reports them, prints button, stick and pressure changes
to TTY, and fires short operator-triggered rumble pulses on each motor. It
runs for 120 seconds, or until SELECT+START or the system exit request.

## Controls

Each action fires once per fresh press; holding a button never repeats it.

| Button | Action |
| --- | --- |
| SELECT+START | Exit (wins over every other button in the same frame) |
| R1 | Stop: write zero to both motors now |
| L1 | Next connected port (zero and restore the old port first) |
| CROSS | Small motor only (`motor[0]=1`, `motor[1]=0`) |
| SQUARE | Large motor at 64 |
| TRIANGLE | Both motors, large at 128 |
| CIRCLE | Large motor at 255 |

Priority within one frame: exit, stop, next port, then CROSS, SQUARE,
TRIANGLE, CIRCLE. The DS3 small motor is on/off only, so intensity levels
apply to the large motor. Level 128 is only exercised together with the small
motor (TRIANGLE); 64 and 255 run the large motor alone.

Every pulse lasts 200 ms. The deadline is checked on every loop iteration,
whatever the read returns, and a new pulse replaces the current one and
restarts the deadline. The stop is a write of zero after the deadline; a
failed stop is retried for up to 10 loop iterations and then reported.

## Output

- `DS3B_PORT` lines report the selected port, its capability bits and
  whether pressure mode was enabled.
- `DS3B_BUTTONS` prints DIGITAL1/DIGITAL2 on every change.
- `DS3B_ANALOG` prints sticks, and the 12 pressure values when pressure mode
  is on and the record holds them. A line is printed only when a value moves
  more than 2 raw units from the last printed value, and at most 10 times a
  second.
- `DS3B_PULSE` reports each start, replacement, stop and elapsed time.
- `DS3B_SUMMARY` ends the run, followed by `DS3B_FIRST_ERROR` if any API
  call failed, then `DS3B_API_OK` or `DS3B_FAIL`.

`DS3B_API_OK` means the session ended normally and every motor stop and
setting restore succeeded. It is **not** evidence that a motor physically
vibrated or that a pressure value is meaningful; those are operator
observations on hardware.

## Behaviour

`source/pad_lifecycle.cpp` owns everything a controller sample needs before it
interprets input, and is shared with the DS3 motion sample:

- `cellPadInit` and `cellPadEnd` (end is skipped if init failed).
- Port selection from `cellPadGetInfo2`; L1 cycles through connected ports.
- Pressure mode: after checking `CELL_PAD_CAPABILITY_PRESS_MODE`, the prior
  `port_setting` is saved and `cellPadSetPortSetting` turns on
  `CELL_PAD_SETTING_PRESS_ON`, keeping its other bits. The prior value is
  restored when the port is released or the sample exits; a failed restore is
  retried three times and reported.
- Reads: `CELL_OK` with `len == 0` is success with no new data. A record is
  used only if `len` reaches the field being read (4 for buttons, 8 for
  sticks, 20 for pressure); shorter, negative or over-long records are
  counted as malformed. `CELL_PAD_ERROR_NO_DEVICE` is a disconnect; any other
  error ends the session.
- Motors: written only when the port reports `CELL_PAD_CAPABILITY_ACTUATOR`.
  Zero is written before switching ports, on exit and on error. Nothing is
  written to a disconnected port.
- Reconnect: the port is acquired again (capabilities, pressure) and buttons
  already held are ignored until released, so no stale edge fires.

`samples/sysutil/hello-ppu-pad` already shows the `CellPadInfo2` capability
dump, the sysutil callback and a fixed both-motor pulse. This sample adds
port selection, pressure mode with restore, length-checked records, separate
small and large motor pulses with deadlines, reconnect handling and
rate-limited stick and pressure output.

## Build

Set `PS3DEV` (and `PS3DK`) to the installed SDK and configure:

```sh
cmake -S samples/input/ds3-bench -B "$PS3_BUILD_ROOT/ds3-ilp32" -G Ninja \
  --toolchain "$PWD/cmake/ps3-ppu-toolchain.cmake"
cmake --build "$PS3_BUILD_ROOT/ds3-ilp32"
```

Use `ps3-ppu-toolchain-lp64.cmake` and a separate build directory for LP64.

## Host tests

`host-tests/` builds the portable logic natively against a scripted fake pad
and a fake clock, and runs eleven CTest cases: truncated records, no new
data, failed actuator writes, failed restore, disconnect and reconnect, held
buttons, exit paths, missing capabilities and a failed pressure enable,
pulse and session deadlines, the change reporter, and port-switch cleanup
(a failed zero or restore refuses the switch and is retried on exit; a
`NO_DEVICE` read is a disconnect).
`tests/host/ds3-bench-test.sh` configures, builds and runs them.

## Hardware checklist (not yet run)

No hardware result exists for this sample yet. RPCS3 can show API and
lifecycle behaviour only; it cannot show a motor or pressure sensor working.
When a hardware run is requested:

1. Run the matched no-input idle control first, with the same firmware and
   HEN state: record duration and outcome. HEN can crash on its own, so a
   hang without this baseline is not attributed to the sample.
2. Pressure: press each pressure button slowly; values should rise from 0.
3. Large motor at 64, 255 (SQUARE, CIRCLE) and with the small motor at 128
   (TRIANGLE); small motor alone (CROSS). Note whether each felt different.
4. Stop: start CIRCLE and press R1 at once; the motor should stop early.
5. Port switch with two controllers (L1); reconnect by unplugging one.
6. Exit with SELECT+START and with the PS button; check `DS3B_API_OK`.
