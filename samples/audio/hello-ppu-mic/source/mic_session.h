#pragma once
#include "mic_meter.h"
namespace mic_sample {
enum class Stage { callback, mic_module, avconf_module, init, open, start };
enum class Result { complete, quit, refused, failed };
constexpr int exit_status(Result r) {
    return r==Result::complete?0:r==Result::quit?3:r==Result::refused?2:1;
}
struct Config { bool generic=false; int device=-1; unsigned channels=4; };
struct Stats {
    int device=-1, first_error=0;
    const char *reason="none";
    unsigned cleanup_failures=0, retained=0, windows=0;
    uint64_t frames=0, bytes=0, elapsed_us=0;
};
struct Ops {
    void *context;
    // acquire returns 0 owned, 1 borrowed (modules only), negative error.
    int (*acquire)(void *, Stage, int device, unsigned channels);
    int (*release)(void *, Stage, int device);
    int (*inventory)(void *);
    int (*attached)(void *, int device); // 0 absent, 1 present, negative error
    int (*type)(void *, int device, int *type); // 2 Eye, 3 USB
    int (*format)(void *, int device, Format *format);
    int (*read)(void *, int device, uint8_t *bytes, unsigned capacity);
    int (*pump)(void *); // 0 continue, 1 quit, negative error
    uint64_t (*now)(void *);
    void (*wait)(void *, uint64_t microseconds);
    Emit emit;
};
Result run(const Ops &, const Config &, Stats &);
const char *stage_name(Stage);
}
