/* Simulated MFC for host tests; see mfc_mock.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mfc_mock.h"
#include "spu_mfcio.h"

uint8_t mfc_mock_memory[MFC_MOCK_MEMORY] __attribute__((aligned(128)));
int mfc_mock_swap32;
void (*mfc_mock_on_reserve)(uint64_t line_ea);
unsigned long mfc_mock_reserves;

static uint64_t reserved_line = UINT64_MAX;
static uint32_t atomic_status;

static void die(const char *what, uint64_t ea, uint32_t size)
{
    printf("FAIL: illegal MFC command: %s (ea 0x%llx, size %u)\n", what,
           (unsigned long long)ea, size);
    exit(3);
}

static void copy(void *dst, const void *src, uint32_t size)
{
    uint32_t i;

    if (!mfc_mock_swap32) {
        memcpy(dst, src, size);
        return;
    }
    for (i = 0; i < size; i += 4) {
        const uint8_t *s = (const uint8_t *)src + i;
        uint8_t *d = (uint8_t *)dst + i;

        d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];
    }
}

/* Any store into the reserved line breaks the reservation. */
static void stored(uint64_t ea, uint32_t size)
{
    if (reserved_line != UINT64_MAX && ea < reserved_line + 128
        && ea + size > reserved_line)
        reserved_line = UINT64_MAX;
}

void mfc_mock_foreign_store(uint64_t ea, const void *bytes, uint32_t size)
{
    memcpy(mfc_mock_memory + ea, bytes, size);
    stored(ea, size);
}

void mfc_mock_command(volatile void *ls, uint64_t ea, uint32_t size,
                      uint32_t tag, uint32_t cmd)
{
    void *local = (void *)(uintptr_t)ls;

    if (ea == 0 || ea + size > MFC_MOCK_MEMORY || ea + size < ea)
        die("out of range", ea, size);
    if (tag > 31)
        die("tag", ea, size);
    if (mfc_mock_swap32 && (size & 3))
        die("swap32 transfer not a word multiple", ea, size);
    switch (cmd) {
    case MFC_GETLLAR_CMD:
    case MFC_PUTLLC_CMD:
    case MFC_PUTLLUC_CMD:
        if ((ea & 127) || ((uintptr_t)local & 127) || size != 128)
            die("line command not on a 128-byte line", ea, size);
        break;
    default:
        if (!(size == 1 || size == 2 || size == 4 || size == 8
              || (size % 16 == 0 && size <= 16384)))
            die("size", ea, size);
        if ((ea & 15) != ((uintptr_t)local & 15) || (size < 16 && (ea & (size - 1))))
            die("alignment", ea, size);
    }

    switch (cmd) {
    case MFC_GET_CMD:
        copy(local, mfc_mock_memory + ea, size);
        break;
    case MFC_PUT_CMD:
        copy(mfc_mock_memory + ea, local, size);
        stored(ea, size);
        break;
    case MFC_GETLLAR_CMD:
        ++mfc_mock_reserves;
        if (mfc_mock_on_reserve)
            mfc_mock_on_reserve(ea);
        copy(local, mfc_mock_memory + ea, 128);
        reserved_line = ea;
        atomic_status = 4;          /* getllar complete */
        break;
    case MFC_PUTLLC_CMD:
        if (reserved_line == ea) {
            copy(mfc_mock_memory + ea, local, 128);
            atomic_status = 0;
        } else {
            atomic_status = MFC_PUTLLC_STATUS;
        }
        reserved_line = UINT64_MAX;
        break;
    case MFC_PUTLLUC_CMD:
        copy(mfc_mock_memory + ea, local, 128);
        stored(ea, 128);
        atomic_status = 2;          /* putlluc complete */
        break;
    default:
        die("command", ea, size);
    }
}

uint32_t mfc_mock_atomic_status(void)
{
    return atomic_status;
}
