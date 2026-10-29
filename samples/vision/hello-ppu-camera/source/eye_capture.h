#ifndef EYE_CAPTURE_H
#define EYE_CAPTURE_H
#include <stddef.h>
#include <stdint.h>

enum EyeStage { EYE_LOAD, EYE_INIT, EYE_OPEN, EYE_START,
                EYE_STOP, EYE_CLOSE, EYE_END, EYE_UNLOAD,
                EYE_READ, EYE_PRESENT, EYE_PUMP, EYE_DEADLINE, EYE_FRAME };
enum { EYE_TIMEOUT = -1, EYE_INVALID_FRAME = -2, EYE_BAD_ARGUMENT = -3 };
#define EYE_FRAME_TIMEOUT_US UINT64_C(2000000)
typedef struct EyeFrame {
    const uint8_t *data;
    size_t bytes;
    uint32_t number;
    uint64_t timestamp;
} EyeFrame;
typedef struct EyeStats {
    unsigned fresh, duplicates, displayed, cleanup_failures;
    int first_error;
    enum EyeStage error_stage;
    uint64_t elapsed_us, first_timestamp, last_timestamp;
} EyeStats;
typedef struct EyeOps {
    void *context;
    int (*control)(void *, enum EyeStage);
    int (*read)(void *, EyeFrame *); /* 0 metadata, 1 retry, negative failure */
    int (*present)(void *, const EyeFrame *);
    int (*pump)(void *); /* 0 continue, 1 user exit, negative failure */
    uint64_t (*now)(void *);
    void (*wait)(void *);
} EyeOps;

/* Own only successfully acquired resources. Clean up in reverse order even
 * after failure. Return 0 complete, 1 user exit, negative error; the first
 * error and its stage survive later cleanup errors. */
int eye_capture_run(const EyeOps *, unsigned target, uint64_t budget_us, EyeStats *);
#endif
