#include "judge.h"
#include <stdio.h>
#include <string.h>

uint8_t test_pattern_byte(uint32_t r, uint32_t c)
{
    /* Nonuniform deterministic formula distinct from 0xAA poison */
    uint8_t val = (uint8_t)((r * 37u + c * 59u + 0x13u) & 0xFFu);
    if (val == POISON_BYTE) {
        val = 0x55u;
    }
    return val;
}

int judge_transfer_bytes(const uint8_t *dst_base,
                         uint32_t guard_size,
                         uint32_t width,
                         uint32_t height,
                         uint32_t dst_pitch,
                         uint8_t poison_byte,
                         char *reason_buf,
                         size_t reason_buf_size)
{
    if (!dst_base) {
        if (reason_buf && reason_buf_size > 0)
            snprintf(reason_buf, reason_buf_size, "null destination buffer pointer");
        return -1;
    }

    /* 1. Check front guard band */
    for (uint32_t i = 0; i < guard_size; i++) {
        if (dst_base[i] != poison_byte) {
            if (reason_buf && reason_buf_size > 0) {
                snprintf(reason_buf, reason_buf_size,
                         "front guard corrupted at offset %u (got 0x%02x, expected 0x%02x)",
                         (unsigned)i, (unsigned)dst_base[i], (unsigned)poison_byte);
            }
            return 1;
        }
    }

    /* 2. Check payload and row padding */
    const uint8_t *dst_region = dst_base + guard_size;
    for (uint32_t r = 0; r < height; r++) {
        const uint8_t *row_ptr = dst_region + (r * dst_pitch);
        /* Check payload columns */
        for (uint32_t c = 0; c < width; c++) {
            uint8_t expected = test_pattern_byte(r, c);
            if (row_ptr[c] != expected) {
                if (reason_buf && reason_buf_size > 0) {
                    snprintf(reason_buf, reason_buf_size,
                             "payload mismatch at row %u col %u (got 0x%02x, expected 0x%02x)",
                             (unsigned)r, (unsigned)c, (unsigned)row_ptr[c], (unsigned)expected);
                }
                return 2;
            }
        }
        /* Check row padding columns */
        for (uint32_t c = width; c < dst_pitch; c++) {
            if (row_ptr[c] != poison_byte) {
                if (reason_buf && reason_buf_size > 0) {
                    snprintf(reason_buf, reason_buf_size,
                             "padding corrupted at row %u col %u (got 0x%02x, expected 0x%02x)",
                             (unsigned)r, (unsigned)c, (unsigned)row_ptr[c], (unsigned)poison_byte);
                }
                return 3;
            }
        }
    }

    /* 3. Check back guard band */
    const uint8_t *back_guard = dst_region + (dst_pitch * height);
    for (uint32_t i = 0; i < guard_size; i++) {
        if (back_guard[i] != poison_byte) {
            if (reason_buf && reason_buf_size > 0) {
                snprintf(reason_buf, reason_buf_size,
                         "back guard corrupted at offset %u (got 0x%02x, expected 0x%02x)",
                         (unsigned)i, (unsigned)back_guard[i], (unsigned)poison_byte);
            }
            return 4;
        }
    }

    if (reason_buf && reason_buf_size > 0)
        reason_buf[0] = '\0';
    return 0;
}

int validate_io_mapping(uint32_t io_offset,
                        uint32_t host_size,
                        uint32_t guard_size,
                        uint32_t region_size,
                        uint32_t *out_dst_offset)
{
    uint64_t base = (uint64_t)io_offset;
    uint64_t hsz  = (uint64_t)host_size;
    uint64_t gsz  = (uint64_t)guard_size;
    uint64_t rsz  = (uint64_t)region_size;

    /* Total tested span must fit within allocated host size */
    uint64_t total_span = gsz + rsz + gsz;
    if (total_span > hsz) {
        return -1;
    }

    /* Mapped IO window must not overflow 32-bit representable IO range */
    uint64_t map_end = base + hsz;
    if (map_end > 0xFFFFFFFFULL) {
        return -1;
    }

    /* Destination payload start must be representable */
    uint64_t dst_start = base + gsz;
    if (dst_start > 0xFFFFFFFFULL) {
        return -1;
    }

    /* Destination payload end must not exceed mapped window or 32-bit space */
    uint64_t dst_end = dst_start + rsz;
    if (dst_end > map_end || dst_end > 0xFFFFFFFFULL) {
        return -1;
    }

    /* Full test window end (including back guard) must fit within mapped backing */
    uint64_t full_end = base + total_span;
    if (full_end > map_end || full_end > 0xFFFFFFFFULL) {
        return -1;
    }

    if (out_dst_offset) {
        *out_dst_offset = (uint32_t)dst_start;
    }
    return 0;
}

poll_outcome_t poll_label_progress(volatile const uint32_t *label,
                                   uint32_t target_val,
                                   get_progress_val_fn get_fifo_progress,
                                   sleep_us_fn sleep_us,
                                   uint32_t interval_us,
                                   uint32_t max_stagnant_polls,
                                   void *user_data)
{
    if (!label || max_stagnant_polls == 0) {
        return POLL_INVALID_ARG;
    }

    uint32_t last_label = *label;
    uint32_t last_fifo = get_fifo_progress ? get_fifo_progress(user_data) : 0;
    uint32_t stagnant_count = 0;

    while (1) {
        if (*label == target_val) {
            return POLL_SUCCESS;
        }

        uint32_t cur_label = *label;
        uint32_t cur_fifo = get_fifo_progress ? get_fifo_progress(user_data) : 0;

        int progressed = 0;
        if (cur_label != last_label) {
            last_label = cur_label;
            progressed = 1;
        }
        if (get_fifo_progress && (cur_fifo != last_fifo)) {
            last_fifo = cur_fifo;
            progressed = 1;
        }

        if (progressed) {
            stagnant_count = 0;
        } else {
            stagnant_count++;
            if (stagnant_count >= max_stagnant_polls) {
                return POLL_NO_PROGRESS;
            }
        }

        if (sleep_us && interval_us > 0) {
            sleep_us(interval_us, user_data);
        }
    }
}
