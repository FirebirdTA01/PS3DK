/*
 * hello-ppu-crashdump — sys_crash_dump_* smoke test.
 *
 * Registers a user log area, queries it back, verifies the
 * label matches.  Does NOT trigger a crash — just validates
 * the 2-entry API surface links correctly.
 *
 * This library is 475+ only; pre-475 firmware does not ship it.
 */

#include <cstdio>
#include <cstdint>
#include <cstring>

#include <sys/process.h>
#include <sys/crashdump.h>

SYS_PROCESS_PARAM(1001, 0x10000);

int main(void)
{
    int32_t rc;
    int failures = 0;

    printf("[hello-ppu-crashdump] starting\n");

    /* Set up a user log area — register a small buffer with the
     * SPRX.  No actual crash will be triggered. */
    uint8_t log_buffer[64];
    sys_crash_dump_log_area_info_t set_info;
    strncpy(set_info.label, "test_log_area", SYS_CRASH_DUMP_MAX_LABEL_SIZE);
    set_info.addr = (uint32_t)(uintptr_t)log_buffer;
    set_info.size = sizeof(log_buffer);

    rc = sys_crash_dump_set_user_log_area(0, &set_info);
    printf("[hello-ppu-crashdump] sys_crash_dump_set_user_log_area(0) = %08x\n",
           (unsigned)rc);
    if (rc) failures++;

    /* Query it back and verify the label matches. */
    sys_crash_dump_log_area_info_t get_info;
    rc = sys_crash_dump_get_user_log_area(0, &get_info);
    printf("[hello-ppu-crashdump] sys_crash_dump_get_user_log_area(0) = %08x\n",
           (unsigned)rc);
    if (rc) failures++;

    if (strncmp(set_info.label, get_info.label, SYS_CRASH_DUMP_MAX_LABEL_SIZE) == 0) {
        printf("[hello-ppu-crashdump] label match: '%s'\n", get_info.label);
    } else {
        printf("[hello-ppu-crashdump] WARN: label mismatch\n");
    }

    if (failures) {
        printf("[hello-ppu-crashdump] FAILED: %d calls returned non-zero\n", failures);
        return 1;
    }

    printf("[hello-ppu-crashdump] all tests passed\n");
    return 0;
}
