// The same conversions compiled as C++ (see widen.c).
#include <cstdint>

extern "C" {
unsigned long long widen_u64_cxx (void *p) { return (unsigned long long) p; }
long long widen_s64_cxx (void *p) { return reinterpret_cast<long long> (p); }
std::uint64_t widen_via_uintptr_cxx (void *p) { return (std::uint64_t) reinterpret_cast<std::uintptr_t> (p); }
std::int64_t widen_via_intptr_cxx (void *p) { return (std::int64_t) reinterpret_cast<std::intptr_t> (p); }
}
