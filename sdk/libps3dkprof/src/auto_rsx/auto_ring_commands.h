#ifndef PS3DK_PROF_AUTO_RING_COMMANDS_H
#define PS3DK_PROF_AUTO_RING_COMMANDS_H
#include <stddef.h>
#include <stdint.h>

#define auto_ring_shadow_scan ps3dk_prof__auto_ring_shadow_scan
#define auto_ring_restore ps3dk_prof__auto_ring_restore
#define auto_ring_copy_packet ps3dk_prof__auto_ring_copy_packet
#define auto_ring_tail_take ps3dk_prof__auto_ring_tail_take
#define auto_ring_tail_release ps3dk_prof__auto_ring_tail_release

enum { AUTO_RING_RESTORE_WORDS=18, AUTO_RING_FLUSH_WORDS=28+AUTO_RING_RESTORE_WORDS };
typedef struct auto_ring_shadow {
    /* IN, OUT, source/destination offsets, pitches, length, rows, format.
     * No execution-trigger register is ever restored. */
    uint32_t value[9],known,seen,invalid;
    uint64_t words; /* decoder cost, including skipped other-engine packets */
} auto_ring_shadow;

/* Scan exactly n complete, unpublished owner words. Each header is examined;
 * payload for other subchannels is skipped. No read beyond n, allocation,
 * recursive CALL traversal, guessed object binding or firmware defaults. The
 * runtime must exclude its OWN injected packets from this application shadow.
 * Unknown flow/binding or malformed packets invalidates the shadow permanently. */
int auto_ring_shadow_scan(auto_ring_shadow *,const uint32_t *,size_t n);
/* SIZE_MAX on invalid state/insufficient capacity, with no stores. An untouched
 * or partially configured application only restores the fields it established.
 * Applications must explicitly establish mode/format before depending on them. */
size_t auto_ring_restore(const auto_ring_shadow *,uint32_t *,size_t capacity);
/* The tested 12-word M2MF packet, report DMA 0x66626660 -> MAIN 0xfeed0001.
 * Returns 0 without stores for invalid bounds, 12 otherwise. */
size_t auto_ring_copy_packet(uint32_t *,uint32_t destination,uint32_t report,uint32_t bytes);

typedef struct auto_ring_tail { uintptr_t begin,physical,visible; uint32_t active; } auto_ring_tail;
/* Integer-address checks; runtime assigns the validated end back through its
 * ABI-correct context pointer type. requested_words includes the whole admitted
 * draw and markers. No callback, polling, or implicit wrap. */
int auto_ring_tail_take(auto_ring_tail *,uintptr_t begin,uintptr_t current,uintptr_t *end,
                        uint32_t credit_words,uint64_t requested_words);
/* Restore only the exact context extent we shortened. Caller emits at CURRENT,
 * then any FIFO wrap must recompute its JUMP address from the new current. */
int auto_ring_tail_release(auto_ring_tail *,uintptr_t begin,uintptr_t current,uintptr_t *end);
#endif
