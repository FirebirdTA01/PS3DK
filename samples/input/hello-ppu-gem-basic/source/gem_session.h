#ifndef GEM_BASIC_SESSION_H
#define GEM_BASIC_SESSION_H
#include <cstdint>

namespace gem_basic {
constexpr std::uint64_t connect_us=10000000, session_us=30000000,
                        stale_us=2000000, pulse_us=250000;
constexpr std::uint16_t move_button=1u<<2, start_button=1u<<3, cross_button=1u<<6;
enum class Stage { callback, load_spurs, load_gem, spu_init, allocate, spurs_init,
    gem_init, info, read, pump, rumble, colour, clock, connect_timeout, stale,
    invalid_sample, gem_end, spurs_end, free_memory, unload_gem, unload_spurs,
    unregister_callback };
enum class Result { complete, quit, refused, failed };
struct Sample {
    std::uint16_t buttons=0, trigger=0;
    float accel[3]{}, gyro[3]{};
    std::uint64_t timestamp=0;
    std::uint32_t counter=0;
};
struct Stats {
    unsigned fresh=0, duplicates=0, input_changes=0, cleanup_failures=0;
    int first_error=0;
    Stage error_stage=Stage::callback;
    std::uint64_t elapsed_us=0;
    bool retained_resources=false;
};
struct Ops {
    void *context;
    int (*control)(void *, Stage); // zero only on success; atomic acquisition
    int (*connected)(void *, bool *);
    int (*read)(void *, Sample *); // output valid only on zero
    int (*rumble)(void *, std::uint8_t);
    int (*colour)(void *, unsigned); // 0 off, 1 blue, 2 red, 3 green
    int (*pump)(void *); // 0 continue, 1 exit request, other = error
    std::uint64_t (*now)(void *);
    void (*wait)(void *, std::uint64_t maximum_us);
    void (*observe)(void *, const Sample &, bool changed); // optional
};
// No camera calls. Owned resources are released only after their users end.
Result run(const Ops &, Stats &);
const char *stage_name(Stage);
}
#endif
