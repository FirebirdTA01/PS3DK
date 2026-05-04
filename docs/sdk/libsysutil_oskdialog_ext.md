# libsysutil_oskdialog_ext (cellOskDialogExt) — extended OSK dialog

| Property | Value |
|---|---|
| Header | `<cell/sysutil_oskdialog_ext.h>` |
| Stub archive | `libsysutil_oskdialog_ext_stub.a` (nidgen) |
| SPRX module | `cellOskExtUtility` |
| Entry points | 17 (16 in pre-475 firmware) |
| Required sysmodules | `CELL_SYSMODULE_SYSUTIL` (0x0015) + `CELL_SYSMODULE_SYSUTIL_OSK_EXT` (0x003b) |

## Surface

cellOskDialogExt layers extended behaviour on top of the base cellOskDialog surface: input-word confirm filtering, hardware-keyboard event hooks, force-finish callbacks, half-byte kana toggle, clipboard, pointer display, and visual scaling / colour / input-device lock controls.

| Function | Signature |
|---|---|
| `cellOskDialogExtRegisterConfirmWordFilterCallback` | `int(cellOskDialogConfirmWordFilterCallback)` |
| `cellOskDialogExtRegisterKeyboardEventHookCallback` | `int(uint16_t hookEventMode, cellOskDialogHardwareKeyboardEventHookCallback)` |
| `cellOskDialogExtRegisterKeyboardEventHookCallbackEx` | `int(uint16_t hookEventMode, cellOskDialogHardwareKeyboardEventHookCallback)` |
| `cellOskDialogExtSetInitialScale` | `int(float initialScale)` |
| `cellOskDialogExtUpdateInputText` | `int(void)` |
| `cellOskDialogExtInputDeviceLock` | `int(void)` |
| `cellOskDialogExtInputDeviceUnlock` | `int(void)` |
| `cellOskDialogExtSendFinishMessage` | `int(CellOskDialogFinishReason)` |
| `cellOskDialogExtRegisterForceFinishCallback` | `int(cellOskDialogForceFinishCallback)` |
| `cellOskDialogExtSetBaseColor` | `int(float r, float g, float b, float a)` |
| `cellOskDialogExtEnableHalfByteKana` | `int(void)` |
| `cellOskDialogExtDisableHalfByteKana` | `int(void)` |
| `cellOskDialogExtAddJapaneseOptionDictionary` | `int(const char **filePath)` |
| `cellOskDialogExtAddOptionDictionary` | `int(const CellOskDialogImeDictionaryInfo*)` |
| `cellOskDialogExtEnableClipboard` | `int(void)` |
| `cellOskDialogExtUpdatePointerDisplayPos` | `int(CellOskDialogPoint pos)` |
| `cellOskDialogExtSetPointerEnable` | `int(bool enable)` |

### Callback typedefs

| Typedef | Signature |
|---|---|
| `cellOskDialogConfirmWordFilterCallback` | `int(uint16_t *pConfirmString, int32_t wordLength)` → returns `CellOskDialogFilterCallbackReturnValue` |
| `cellOskDialogHardwareKeyboardEventHookCallback` | `bool(CellOskDialogKeyMessage *keyMessage, uint32_t *action, void *pActionInfo)` |
| `cellOskDialogForceFinishCallback` | `bool(void)` |

## Error codes

Errors are in the base cellOskDialog range (`CELL_ERROR_FACILITY_SYSUTIL_OSK`):

| Error | Value |
|---|---|
| `CELL_OSKDIALOG_ERROR_IME_ALREADY_IN_USE` | 0x8002b501 |
| `CELL_OSKDIALOG_ERROR_GET_SIZE_ERROR` | 0x8002b502 |
| `CELL_OSKDIALOG_ERROR_UNKNOWN` | 0x8002b503 |
| `CELL_OSKDIALOG_ERROR_PARAM` | 0x8002b504 |

## Initialisation lifecycle

1. Load sysmodules: `cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL)` + `cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_OSK_EXT)`.
2. Configure visual properties: `cellOskDialogExtSetInitialScale`, `cellOskDialogExtSetBaseColor`.
3. Register callbacks: `cellOskDialogExtRegisterConfirmWordFilterCallback`, `cellOskDialogExtRegisterKeyboardEventHookCallback{,Ex}`, `cellOskDialogExtRegisterForceFinishCallback`.
4. Toggle input features: `cellOskDialogExtEnableHalfByteKana` / `DisableHalfByteKana`, `cellOskDialogExtEnableClipboard`.
5. (Requires open dialog) `cellOskDialogExtUpdateInputText`, `cellOskDialogExtSendFinishMessage`.

## Notable ABI quirks

### 1. SDK-version delta

`cellOskDialogExtRegisterKeyboardEventHookCallbackEx` is a 475+ addition. Pre-475 firmware revisions ship only 16 entry points. Downstream consumers targeting older firmware should guard the Ex variant with a runtime-version check.

### 2. NULL callback rejection

All `Register*Callback` functions reject `NULL` `pCallback` with `CELL_OSKDIALOG_ERROR_PARAM` (0x8002b504). Pass a valid no-op function pointer:

```c
static int osk_word_filter_cb(uint16_t *str, int32_t len) {
    (void)str; (void)len;
    return CELL_OSKDIALOG_NOT_CHANGE;
}
static bool osk_kbd_hook_cb(CellOskDialogKeyMessage *msg, uint32_t *act, void *info) {
    (void)msg; (void)act; (void)info;
    return false;
}
static bool osk_force_finish_cb(void) { return false; }
```

### 3. RegisterKeyboardEventHookCallback hookEventMode bitmask range

| Variant | Valid range | Flag bits |
|---|---|---|
| Non-Ex | [1..3] (`0x01` – `0x03`) | FUNCTION_KEY (0x01), ASCII_KEY (0x02) |
| Ex | [1..7] (`0x01` – `0x07`) | Above + ONLY_MODIFIER (0x04) |

Passing 0 or a value outside the range returns `CELL_OSKDIALOG_ERROR_PARAM`.

### 4. const char **filePath LP64 width trap

`cellOskDialogExtAddJapaneseOptionDictionary` takes a `const char **filePath` — a NULL-terminated array of effective addresses the SPRX reads with 32-bit pointers. Under LP64 a `const char *paths[]` array has 8-byte elements; PowerPC big-endian means the SPRX's `lwz` at offset 0 reads the high half (zero) and terminates immediately.

**Workaround:** construct an explicit `uint32_t` array:

```c
uint32_t paths_ea[] = {
    (uint32_t)(uintptr_t)"/dev_hdd0/.../dict_a.bin",
    (uint32_t)(uintptr_t)"/dev_hdd0/.../dict_b.bin",
    0  /* NULL terminator */
};
cellOskDialogExtAddJapaneseOptionDictionary((const char **)paths_ea);
```

### 5. SendFinishMessage requires an open dialog

`cellOskDialogExtSendFinishMessage` returns `CELL_OSKDIALOG_ERROR_PARAM` (or error range) when no OSK dialog is open. It is not suitable for smoke testing without a preceding `cellOskDialogLoadAsync` → open lifecycle.

## Sample

`samples/sysutil/hello-ppu-osk-ext/` — PPU smoke test validated in RPCS3: loads SYSUTIL + OSK_EXT sysmodules, exercises 11 entry points (SetInitialScale, SetBaseColor, Enable/DisableHalfByteKana, 3 Register*Callback with no-op functions, InputDeviceLock/Unlock, SetPointerEnable). All return CELL_OK.
