# Move buttons and inertial input without a camera

This C++17 sample creates a SPURS instance, initializes `cell/gem.h`, waits
up to 10 seconds for Move slot 0, then reads the latest inertial records for
30 seconds. It does not open a camera, submit tracking frames or request pose.
Buttons, trigger, accelerometer and gyro are printed to TTY, with timestamps,
counters and input-change markers. Connect a paired Move before starting.

Controls:

- Move button: one rumble pulse at intensity 96, with a 250 ms stop-request deadline.
  Releasing the button stops it early; holding does not repeat it.
- Cross: cycle the sphere between blue, red and green.
- Start or the system exit callback: end the session and clean up.

IMU values, rumble and sphere colour are verified on hardware only.
They have **not yet been verified on hardware for this sample**. RPCS3 rows
are labelled **API/input/lifecycle only**. A successful read or a matching
rumble/colour API readback does not prove a moving sensor, a vibrating motor
or a lit sphere. `GEM_API_OK` means the bounded read session and cleanup
completed; it does not require the user to have changed any inputs. Inspect
`input_changes` and `GEM_INPUT` observations separately.

## Build

Set `PS3DEV` to the installed SDK and configure from the repository root:

```sh
cmake -S samples/input/hello-ppu-gem-basic -B "$PS3_BUILD_ROOT/gem-ilp32" \
  --toolchain "$PWD/cmake/ps3-ppu-toolchain.cmake"
cmake --build "$PS3_BUILD_ROOT/gem-ilp32" --parallel 2
```

Use `ps3-ppu-toolchain-lp64.cmake` and a separate build directory for LP64.
Define `PS3_BUILD_ROOT` first, for example `$HOME/ps3tc/build`. The installed
SDK must include the GEM headers and stub archive; repository validation
uses current repository headers explicitly. The `.elf`, `.self` and
`.fake.self` outputs are explicitly placed in the build directory.

`tests/host/gem-session-test.sh` checks the portable session logic.
`tests/sdk/gem-basic-build-test.sh --ps3dev "$PS3DEV"` builds both ABIs,
retains outputs, checks the caller objects' GEM/SPURS references and checks
all sample sources for added, removed or modified files, including ignored
outputs. The linked GEM archive includes unused tracking stubs in the same
member; their presence is not a call to them.

## Behaviour and cleanup

The sample polls at 10 ms intervals and refuses a two-second sensor stall,
disconnection, backwards timestamps/counters or malformed sensor records.
Repeats are not new samples; unsigned counter rollover is supported.
Aligned state objects and PRX-addressable work buffers are used in both ABIs.
There is no camera calibration, pose estimate or sensor fusion.

The stop request is issued at the first scheduling/API return after its
deadline, with the actual elapsed request time logged for each pulse. The
requested sleep is capped at the remaining pulse interval. Slow API calls
or scheduling can delay the request; there is no claim about when the motor
physically stops. A stuck library call remains a watchdog failure.

Every successful acquisition has cleanup: request rumble zero and sphere
off, end GEM, finalize SPURS, release memory, unload owned modules and
unregister the callback. The first failure survives later cleanup failures.
If GEM or SPURS cannot end, memory/provider modules that may still be in use
are retained until process exit. A runtime harness must enforce an outer
timeout (60 seconds suggested); a hung library call is a failed row.

Final markers are `GEM_API_OK`, `GEM_QUIT`, `GEM_REFUSED` or `GEM_FAIL`.
Refusal and failure exit nonzero. Quit is successful only when cleanup also
succeeds. Cleanup results and retained-resource status are always reported.

## Emulator and hardware checks

At RPCS3 `54014a7d`, Fake Move takes its first controller from pad player 7,
with default Move=R1 and trigger=R2. Record the private pad mapping, Move and
Camera configuration with every row. Mouse supplies fixed gravity and zero
gyro, and its trigger is binary; Fake sensor values depend on its pad backend.
See [the pinned inertial query](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/Cell/Modules/cellGem.cpp#L3241-L3318)
and [input mappings](https://github.com/RPCS3/rpcs3/blob/54014a7d/rpcs3/Emu/Io/gem_config.h#L81-L130).

Runtime checks are opt-in, since the ordinary regression configuration does
not guarantee a connected Move. Use a visible window and a fresh private
configuration/output directory for each ABI and each attempt. Planned rows
use Move=Fake with recorded player-7 mapping, first Camera=Fake and then
Camera=Null. Neither row should open a camera. Also check disconnected Move
refusal and user exit. Use the repository RPCS3 claim/release scripts and
obtain run approval separately. Stop and report a failed row.

On 2026-10-28, eight RPCS3 rows passed: both ABIs with Fake Move and each
Camera setting, both Null-Move refusal rows, and both guest-exit callback
rows. Every row completed cleanup. The four read sessions observed only
released buttons, zero trigger and zero IMU components; no inputs were
injected. These runs prove connection, idle reads, refusal and lifecycle,
not button transitions, trigger response or physical sensor/output behaviour.

A separately approved PS3 session must record actual button transitions,
intermediate trigger values, acceleration changes on tilt, gyro changes on
rotation, visible sphere colours and felt rumble, with the operator's
observations verbatim. No Eye is required. Until then physical behaviour
remains unproven.
