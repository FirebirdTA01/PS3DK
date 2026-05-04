# libsys_crashdump (sys_crash_dump) — crash dump user log registration

| Property | Value |
|---|---|
| Header | `<sys/crashdump.h>` |
| Stub archive | `libsys_crashdump_stub.a` (nidgen) |
| SPRX module | `sys_crashdump` (LV2 syscall library, not cellXxx) |
| Entry points | 2 |
| Availability | 475+ firmware only; pre-475 does not ship this library |

## Surface

sys_crash_dump provides user-log-area registration for the PS3 crash dump facility. Applications register memory regions (with a label, address, and size) that the kernel includes in crash dump files.

### Function table (2)

| Function | Signature |
|---|---|
| `sys_crash_dump_set_user_log_area` | `int(uint8_t index, sys_crash_dump_log_area_info_t *new_entry)` |
| `sys_crash_dump_get_user_log_area` | `int(uint8_t index, sys_crash_dump_log_area_info_t *entry)` |

The `index` parameter selects one of potentially multiple user log areas (typically 0-based).

## Types

| Type | Fields |
|---|---|
| `sys_crash_dump_log_area_info_t` | `char label[16]` — user-readable label; `uint32_t addr` — effective address of the log buffer; `uint32_t size` — buffer size in bytes |

## Initialisation lifecycle

No init/end pair required. The library is stateless — call `set` at any point during process lifetime to register (or replace) a log area, `get` to retrieve the currently registered entry.

## Notable ABI quirks

### 1. Whole-library version requirement

This library does not exist in pre-475 firmware. Applications targeting older firmware must guard or omit crashdump usage. The two entry points (`sys_crash_dump_set_user_log_area`, `sys_crash_dump_get_user_log_area`) are absent from 3.70 entirely.

### 2. sys_ naming convention (LV2 syscall library)

Unlike most cellSDK libraries which use the `cellXxx*` naming prefix, crashdump uses the `sys_crash_dump_*` LV2-syscall-style convention. The module name is `sys_crashdump` (without underscore between "crash" and "dump"); the function names contain `crash_dump` (with underscore). The stub archive installs as `libsys_crashdump_stub.a`.

### 3. Width-sensitive integers

`sys_crash_dump_log_area_info_t.addr` is declared as `uint32_t` rather than the LP64-native `sys_addr_t` (8 bytes) because the SPRX uses a 32-bit-pointer ABI. Similarly, `size` is `uint32_t` rather than LP64's 8-byte `size_t`. This is the same class of issue as the libdmux width-sensitive fields; see [libdmux.md §1](libdmux.md#1-width-sensitive-integers-in-caller-allocated-structs).

No pointer fields cross the SPRX boundary in the user-allocated struct (addr is an EA stored as uint32_t, not a pointer the SPRX dereferences), so ATTRIBUTE_PRXPTR is not needed.

## Sample

`samples/lv2/hello-ppu-crashdump/` — PPU smoke test: registers a 64-byte user log area at index 0, queries it back, verifies the label round-trips. Does not trigger an actual crash.
