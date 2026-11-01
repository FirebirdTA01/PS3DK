# DualShock 3 buttons, pressure and rumble bench

This C++17 sample selects a connected controller, turns on pressure-sensitive
buttons and the Sixaxis sensors where the port reports them, shows every
button, both sticks, the pressure values and the raw Sixaxis readings on
screen (and logs changes to TTY), and fires short operator-triggered rumble
pulses on each motor. It runs for 120 seconds, or until SELECT+START or the
system exit request.

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

Every pulse lasts 200 ms. The loop runs every 10 ms (plus the time of its
API calls), and the deadline is checked on every iteration whatever the read
returns, so a stop is requested within about one iteration of the deadline.
A new pulse replaces the current one and restarts the deadline. The stop is
a write of zero after the deadline; a failed stop is retried for up to 10
loop iterations, then the screen keeps a STOP FAILED warning.

## Screen

The page lists the controls at the top, then:

- every button, highlighted while held;
- both sticks, when the record holds them;
- the 12 pressure values in two rows, when pressure mode is on and the record
  holds them; otherwise why not (not supported, enable failed, or enabled but
  not in this record);
- the Sixaxis readings as raw 10-bit counts: accelerometer X, Y, Z and one raw
  gyro channel G. Axes, centre and units are not verified on hardware, and
  the page says so;
- the last rumble command the API accepted on this connection (not a measured
  motor state): "no command sent" until one is, or "unknown" while the
  controller is disconnected, since no stop can be sent;
- record counters, time left and the last event.

Values belong to one connection of one controller: after a port change, a
disconnect or a reconnect, the page shows "Waiting for the first record"
until a new record arrives.

`source/display.c` draws the page with the debug font (the setup of
`samples/gcm/hello-ppu-dbgfont`). It never blocks the loop: if the previous
flip has not completed, the frame is skipped, so pad input, motor deadlines
and exit keep running even if presentation stalls. Its own polls at open and
close stop at a one-second monotonic deadline; the GCM and OS calls inside
them are not interruptible. If the display cannot open, or a submission
fails, the sample continues with TTY output only and says so.

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
  `CELL_PAD_SETTING_PRESS_ON`, keeping its other bits. Sixaxis works the same
  way: after `CELL_PAD_CAPABILITY_SENSOR_MODE`, `CELL_PAD_SETTING_SENSOR_ON`
  is added. The prior value is restored when the port is released or the
  sample exits; a failed restore is retried three times and reported.
- Reads: `CELL_OK` with `len == 0` is success with no new data. A record is
  used only if `len` reaches the field being read (4 for buttons, 8 for
  sticks, 20 for pressure, 24 for Sixaxis); shorter, negative or over-long records are
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
and a fake clock, and runs twelve CTest cases: truncated records, no new
data, failed actuator writes, failed restore, disconnect and reconnect, held
buttons, exit paths, missing capabilities and a failed pressure enable,
pulse and session deadlines, the change reporter, port-switch cleanup (a
failed zero or restore refuses the switch and is retried on exit; a
`NO_DEVICE` read is a disconnect), and the screen: its content, row widths,
cleared values after a controller change, and a stalled or failing display
that must not stop motor deadlines or exit. They check the text, not how it
looks; the layout needs a visual check on the console.
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
6. Screen: every line readable and inside the picture at the console's
   output resolution; held buttons highlight; the page keeps updating.
7. Sixaxis: hold still, then tilt and turn; note which raw values move for
   each motion. This records the axes; it does not calibrate them.
8. Exit with SELECT+START and with the PS button; check `DS3B_API_OK`.
