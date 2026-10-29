#pragma once
#include <cstddef>
#include <cstdint>

namespace mic_sample {
struct Format { unsigned channels = 0, subframe = 0, bits = 0, endian = 0, rate = 0; };
struct Window {
    uint32_t frames = 0;
    uint32_t peak[4]{}, rms[4]{}, clipped[4]{};
};
using Emit = void (*)(void *, const Window &);
class Meter {
public:
    explicit Meter(Format format) : format_(format) {}
    bool valid() const;
    bool push(const uint8_t *bytes, size_t size, Emit emit, void *context);
    bool finish(Emit emit, void *context);
    uint64_t frames() const { return frames_; }
    size_t pending() const { return pending_; }
private:
    void frame(Emit emit, void *context);
    void flush(Emit emit, void *context);
    Format format_;
    uint8_t carry_[8]{};
    size_t pending_ = 0;
    uint64_t frames_ = 0, squares_[4]{};
    Window window_{};
};
}
