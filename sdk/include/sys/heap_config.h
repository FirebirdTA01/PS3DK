/* Application configuration for the process-owned newlib malloc arena. */
#ifndef PS3TC_SYS_HEAP_CONFIG_H
#define PS3TC_SYS_HEAP_CONFIG_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern const uint64_t __ps3tc_heap_size;
extern const unsigned char __ps3tc_heap_config_anchor;
#ifdef __cplusplus
}
#endif

/* Use once at global scope in a C or C++ application translation unit:
 *     PS3TC_HEAP_SIZE(UINT64_C(128) * 1024 * 1024);
 * Units are bytes, rounded UP to whole 1 MiB pages. Zero, rounding overflow,
 * sizes outside the address/syscall limits and unavailable memory are refused
 * at startup with one TTY diagnostic and process exit status 1.
 * Without this macro the allocation size remains 64 MiB and the page size
 * remains SYS_MEMORY_PAGE_SIZE_1M, as before.
 *
 * An ordinary app object needs no linker option. If this definition lives
 * inside a static library, force its member into the link with
 *     -Wl,-u,__ps3tc_heap_config_anchor
 * Strong symbols in unextracted archive members cannot override the SDK's
 * weak default. Exactly one application definition is permitted.
 * See docs/abi/newlib-heap.md for examples and failure diagnostics.
 */
#define PS3TC_HEAP_SIZE(bytes) \
    const uint64_t __ps3tc_heap_size = (uint64_t)(bytes); \
    const unsigned char __ps3tc_heap_config_anchor = 0
#endif
