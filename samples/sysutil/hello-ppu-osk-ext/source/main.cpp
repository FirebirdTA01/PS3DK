/*
 * hello-ppu-osk-ext — cellOskDialogExt* extended OSK smoke test.
 *
 * Exercises the 17-entry ext surface without requiring an open OSK
 * dialog (RPCS3 HLE returns CELL_OK for most calls).  Loads both
 * the base sysutil module and the OSK ext module before entry.
 */

#include <cstdio>
#include <cstdint>

#include <sys/process.h>
#include <cell/error.h>
#include <cell/sysmodule.h>
#include <cell/sysutil_oskdialog_ext.h>

SYS_PROCESS_PARAM(1001, 0x10000);

/* No-op callbacks for register-style entry points that reject NULL. */
static int osk_word_filter_cb(uint16_t *str, int32_t len)
{
    (void)str; (void)len;
    return CELL_OSKDIALOG_NOT_CHANGE;
}

static bool osk_kbd_hook_cb(CellOskDialogKeyMessage *msg, uint32_t *act, void *info)
{
    (void)msg; (void)act; (void)info;
    return false;
}

static bool osk_force_finish_cb(void)
{
    return false;
}

int main(void)
{
    int32_t rc;
    int failures = 0;

    printf("[hello-ppu-osk-ext] starting\n");

    /* Load dependencies.  CELL_SYSMODULE_SYSUTIL is the base sysutil
     * framework; CELL_SYSMODULE_SYSUTIL_OSK_EXT is the ext backend.
     * Tolerate CELL_SYSMODULE_LOADED / CELL_SYSMODULE_ERROR_FATAL. */
    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL);
    printf("[hello-ppu-osk-ext] cellSysmoduleLoadModule(SYSUTIL) = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL) {
        printf("[hello-ppu-osk-ext] FAIL: sysutil module load\n");
        return 1;
    }

    rc = cellSysmoduleLoadModule(CELL_SYSMODULE_SYSUTIL_OSK_EXT);
    printf("[hello-ppu-osk-ext] cellSysmoduleLoadModule(OSK_EXT) = %08x\n", (unsigned)rc);
    if (rc != CELL_OK && rc != CELL_SYSMODULE_LOADED && rc != CELL_SYSMODULE_ERROR_FATAL) {
        printf("[hello-ppu-osk-ext] FAIL: osk_ext module load\n");
        return 1;
    }

    /* --- SetInitialScale ---------------------------------------- */
    rc = cellOskDialogExtSetInitialScale(0.9f);
    printf("[hello-ppu-osk-ext] cellOskDialogExtSetInitialScale(0.9) = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- SetBaseColor ------------------------------------------- */
    rc = cellOskDialogExtSetBaseColor(1.0f, 1.0f, 1.0f, 1.0f);
    printf("[hello-ppu-osk-ext] cellOskDialogExtSetBaseColor(1,1,1,1) = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- EnableHalfByteKana ------------------------------------- */
    rc = cellOskDialogExtEnableHalfByteKana();
    printf("[hello-ppu-osk-ext] cellOskDialogExtEnableHalfByteKana() = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- DisableHalfByteKana ------------------------------------ */
    rc = cellOskDialogExtDisableHalfByteKana();
    printf("[hello-ppu-osk-ext] cellOskDialogExtDisableHalfByteKana() = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- RegisterConfirmWordFilterCallback ----------------------- */
    rc = cellOskDialogExtRegisterConfirmWordFilterCallback(osk_word_filter_cb);
    printf("[hello-ppu-osk-ext] RegisterConfirmWordFilterCallback = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- RegisterKeyboardEventHookCallback ----------------------- */
    uint16_t mode_basic = CELL_OSKDIALOG_EVENT_HOOK_TYPE_FUNCTION_KEY |
                          CELL_OSKDIALOG_EVENT_HOOK_TYPE_ASCII_KEY;       /* 0x03 */
    rc = cellOskDialogExtRegisterKeyboardEventHookCallback(mode_basic, osk_kbd_hook_cb);
    printf("[hello-ppu-osk-ext] RegisterKeyboardEventHookCallback(0x03) = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- RegisterKeyboardEventHookCallbackEx --------------------- */
    uint16_t mode_ex = CELL_OSKDIALOG_EVENT_HOOK_TYPE_FUNCTION_KEY |
                       CELL_OSKDIALOG_EVENT_HOOK_TYPE_ASCII_KEY |
                       CELL_OSKDIALOG_EVENT_HOOK_TYPE_ONLY_MODIFIER;      /* 0x07 */
    rc = cellOskDialogExtRegisterKeyboardEventHookCallbackEx(mode_ex, osk_kbd_hook_cb);
    printf("[hello-ppu-osk-ext] RegisterKeyboardEventHookCallbackEx(0x07) = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- RegisterForceFinishCallback ----------------------------- */
    rc = cellOskDialogExtRegisterForceFinishCallback(osk_force_finish_cb);
    printf("[hello-ppu-osk-ext] RegisterForceFinishCallback = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- InputDeviceLock ----------------------------------------- */
    rc = cellOskDialogExtInputDeviceLock();
    printf("[hello-ppu-osk-ext] cellOskDialogExtInputDeviceLock() = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- InputDeviceUnlock --------------------------------------- */
    rc = cellOskDialogExtInputDeviceUnlock();
    printf("[hello-ppu-osk-ext] cellOskDialogExtInputDeviceUnlock() = %08x\n", (unsigned)rc);
    if (rc) failures++;

    /* --- SetPointerEnable ---------------------------------------- */
    rc = cellOskDialogExtSetPointerEnable(true);
    printf("[hello-ppu-osk-ext] cellOskDialogExtSetPointerEnable(true) = %08x\n", (unsigned)rc);
    if (rc) failures++;

    if (failures) {
        printf("[hello-ppu-osk-ext] FAILED: %d calls returned non-zero\n", failures);
        return 1;
    }

    printf("[hello-ppu-osk-ext] all tests passed\n");
    return 0;
}
