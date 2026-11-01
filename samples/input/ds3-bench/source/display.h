/* Minimal on-screen text page: GCM double buffering plus the debug font.
 * Kept in C, apart from the pad code, because it uses the video and RSX
 * headers.
 *
 * Nothing here blocks the caller's loop: ds3_display_ready() polls the
 * previous flip, and a frame is drawn (begin, text..., end) only when it
 * returns 1. If a flip never completes, frames are skipped while pad input,
 * motor deadlines and exit keep running. Every wait inside open and close is
 * bounded. */
#ifndef DS3_DISPLAY_H
#define DS3_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 on success. On failure nothing is shown and the sample runs TTY-only. */
int ds3_display_open(void);
/* 1 when the previous flip has completed and a new frame may be drawn;
 * 0 while it is pending; -1 after a submission error (display stopped). */
int ds3_display_ready(void);
/* Select the next buffer and clear it. */
void ds3_display_begin(void);
/* x, y in 0..1 screen units; colour is 0xAARRGGBB. */
void ds3_display_text(float x, float y, float scale, uint32_t argb, const char *text);
/* Draw the queued text and request a flip; does not wait for it. Returns 0,
 * or a negative code if the flip could not be queued (display stops). */
int ds3_display_end(void);
/* Polls the RSX against a 1 s monotonic deadline, then returns anyway
 * (the GCM and OS calls themselves are not interruptible). */
void ds3_display_close(void);

#ifdef __cplusplus
}
#endif

#endif
