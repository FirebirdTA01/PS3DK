# Controllers: `cell/pad.h` (DualShock 3)

Link `io` (`libio`) and include `<cell/pad.h>`. This page covers the
standard controller path that `samples/input/ds3-bench` exercises: ports,
records, pressure, Sixaxis and the two motors. The Move motion controller
uses a separate API and is not covered here.

Each statement below says where it comes from:

- **Header**: the declaration in `sdk/include/cell/pad.h`.
- **Sample**: behaviour `samples/input/ds3-bench` implements and checks in its
  host tests (a scripted fake pad, not a console).
- **Hardware**: what has been run on a console. Anything not marked is
  neither measured nor promised.

## Lifecycle

Signatures are from the header; the order and arguments are the sample's.

| Call | Use |
| --- | --- |
| `cellPadInit(max_connect)` | Once at start. The sample passes `CELL_PAD_MAX_PORT_NUM` (7). |
| `cellPadGetInfo2(&info)` | Per-port status, setting, capability and device type. |
| `cellPadGetData(port, &data)` | Read one record. |
| `cellPadSetPortSetting(port, setting)` | Turn pressure and Sixaxis reporting on or off. |
| `cellPadSetActDirect(port, &param)` | Drive the motors. |
| `cellPadEnd()` | Once at exit; skip it if init failed. |

`cellPadGetInfo` (the older `CellPadInfo`) is still declared for PSL1GHT code;
new code uses `cellPadGetInfo2`.

## Ports

- `CELL_PAD_MAX_PORT_NUM` is 7 (header). A port number is the system's
  controller slot, not a physical USB socket; which slot a controller gets is
  the system's choice.
- `info.port_status[port]` has `CELL_PAD_STATUS_CONNECTED` (bit 0) when a
  controller is present and `CELL_PAD_STATUS_ASSIGN_CHANGES` (bit 1) when the
  assignment changed (header).
- `info.device_type[port]` is `CELL_PAD_DEV_TYPE_STANDARD` (0) for a standard
  controller (header).
- `info.device_capability[port]` bits (header):

  | Bit | Constant | Meaning |
  | --- | --- | --- |
  | 0 | `CELL_PAD_CAPABILITY_PS3_CONFORMITY` | PS3-conformant controller |
  | 1 | `CELL_PAD_CAPABILITY_PRESS_MODE` | Pressure-sensitive buttons |
  | 2 | `CELL_PAD_CAPABILITY_SENSOR_MODE` | Sixaxis sensors |
  | 3 | `CELL_PAD_CAPABILITY_HP_ANALOG_STICK` | High-precision sticks |
  | 4 | `CELL_PAD_CAPABILITY_ACTUATOR` | Motors |

Check the capability before using a feature. The sample enables pressure only
with `PRESS_MODE`, Sixaxis only with `SENSOR_MODE`, and writes motors only with
`ACTUATOR` (sample).

## Records

`CellPadData` is `int32_t len` plus `uint16_t button[64]` (header). `len` is
how many `button[]` entries this record holds.

| Result | Meaning (sample) |
| --- | --- |
| `CELL_OK`, `len == 0` | Success with no new data. Keep the previous state, but only within the same connection: after an acquire or reconnect, wait for a new record. |
| `CELL_OK`, `4 <= len <= 64` | A new record. Use only fields below `len`. |
| `CELL_OK`, `len < 0`, `0 < len < 4` or `len > 64` | Malformed; the sample counts and skips it. |
| `CELL_PAD_ERROR_NO_DEVICE` | The controller is gone: treat it as a disconnect. |
| Any other error | The sample ends the session. |

Offsets in `button[]` are from the header. The last column is the sample's
threshold for using a whole group, not a header guarantee: index 8 alone
needs only `len > 8`, but the sample waits until every pressure value is in
the record.

| Index | Content | Sample uses the group at `len` >= |
| --- | --- | --- |
| 2 | `DIGITAL1` bits | 4 |
| 3 | `DIGITAL2` bits | 4 |
| 4, 5 | Right stick X, Y | 8 |
| 6, 7 | Left stick X, Y | 8 |
| 8-11 | Pressure: RIGHT, LEFT, UP, DOWN | 20 |
| 12-15 | Pressure: TRIANGLE, CIRCLE, CROSS, SQUARE | 20 |
| 16-19 | Pressure: L1, R1, L2, R2 | 20 |
| 20-23 | Sixaxis X, Y, Z, G | 24 |

Button bits (header):

| Bit | `DIGITAL1` | `DIGITAL2` |
| --- | --- | --- |
| 0 | SELECT | L2 |
| 1 | L3 | R2 |
| 2 | R3 | L1 |
| 3 | START | R1 |
| 4 | UP | TRIANGLE |
| 5 | RIGHT | CIRCLE |
| 6 | DOWN | CROSS |
| 7 | LEFT | SQUARE |

The sample shows pressure values only while it has set
`CELL_PAD_SETTING_PRESS_ON` and `len >= 20`, and Sixaxis values only while it
has set `CELL_PAD_SETTING_SENSOR_ON` and `len >= 24` (sample). That is the
sample's policy, tested against a fake pad; it does not establish what every
real device returns. The sample shows Sixaxis values as raw 10-bit counts;
their axes, centre and units have not been verified on hardware.

## Pressure and Sixaxis settings

`CELL_PAD_SETTING_PRESS_ON` is bit 1 and `CELL_PAD_SETTING_SENSOR_ON` is bit 2
of the port setting (header). The port setting belongs to the system, so the
sample (sample):

1. saves `info.port_setting[port]` before changing it;
2. sets the bit it needs with `cellPadSetPortSetting`, keeping the other bits;
3. restores the saved value when it releases the port, switches ports or
   exits, with up to three attempts in total, and reports a failure.

## Motors

`CellPadActParam` has `uint8_t motor[2]` (header). The sample follows the
DualShock 3 convention:

- `motor[0]` is the small motor, on/off only: 0 or 1.
- `motor[1]` is the large motor, 0 (off) to 255.

`cellPadSetActDirect` sets a level; it is not a timed pulse. Motors keep
running until zero is written, so the sample (sample):

- attempts the zero write when a pulse's 200 ms monotonic deadline passes;
  loop and API latency apply, so neither the exact duration nor a physical
  stop is guaranteed. A failed stop is retried on later loop iterations;
- writes zero before switching ports, on exit and on error. If the zero write
  or the setting restore still fails after its attempts, the port switch is
  refused and the old port's cleanup is kept and retried at exit;
- never writes to a disconnected port, so after a disconnect the motor state
  is unknown until the controller reconnects;
- writes only when the port reports `CELL_PAD_CAPABILITY_ACTUATOR`.

## Disconnect and reconnect

On `CELL_PAD_ERROR_NO_DEVICE`, or when `cellPadGetInfo2` stops reporting the
port as connected, the sample drops the connection, forgets the last record
and acquires the port again when it returns: capabilities and settings are
read again, and buttons already held at reconnect are ignored until released,
so no stale press fires an action (sample).

## Exit

Register a system callback with `cellSysutilRegisterCallback` and call
`cellSysutilCheckCallback()` every loop iteration. On
`CELL_SYSUTIL_REQUEST_EXITGAME` (0x0101, `cell/sysutil.h`) set a flag, leave the loop,
stop the motors, restore the port settings, call `cellPadEnd()` and return
from `main`.

## Hardware status

`samples/input/ds3-bench` (commit 1db0a9bfe) ran on a retail PS3 with HEN
(firmware 4.93) and was accepted by the operator as a working DS3 test. The
individual checklist items in the sample's README were not recorded
separately: how each rumble level feels, the early stop, pressure values,
Sixaxis axes and readability at the TV resolution. Treat those as unmeasured.
