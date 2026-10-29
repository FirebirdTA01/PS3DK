#ifndef EYE_DISPLAY_H
#define EYE_DISPLAY_H
#include <stddef.h>
#include <stdint.h>
int eye_display_init(void);
int eye_display_present(const uint8_t *rgba, size_t bytes, int (*pump)(void *), void *context);
void eye_display_end(void);
#endif
