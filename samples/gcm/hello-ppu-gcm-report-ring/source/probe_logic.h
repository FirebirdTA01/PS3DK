#ifndef REPORT_RING_PROBE_LOGIC_H
#define REPORT_RING_PROBE_LOGIC_H
#include <stdint.h>
#ifndef RRP_MUTANT
#define RRP_MUTANT 0
#endif
#define RRP_TRIALS 8u
#define RRP_MAX_PAIRS 126u
#define RRP_REPORT_FIRST 512u
#define RRP_SESSION_US UINT64_C(120000000)
#define RRP_DRAIN_US UINT64_C(130000000)
#define RRP_WAIT_US UINT64_C(2000000)
enum { RRP_WAIT, RRP_DONE, RRP_EXPIRED };
static inline int rrp_wait(uint64_t elapsed, uint64_t bound, int passed) {
    if (RRP_MUTANT != 1 && elapsed >= bound) return RRP_EXPIRED;
    return passed ? RRP_DONE : RRP_WAIT;
}
enum { RRP_A1, RRP_A2, RRP_B1, RRP_B2 };
enum { RRP_INVALID, RRP_REFUTED, RRP_INCONCLUSIVE, RRP_SUPPORTED };
static inline unsigned rrp_verdict(unsigned trials, unsigned bad, unsigned invalid, unsigned caught) {
    if (invalid) return RRP_INVALID;
    if (bad) return RRP_REFUTED;
    return trials == RRP_TRIALS && (caught || RRP_MUTANT == 2) ? RRP_SUPPORTED : RRP_INCONCLUSIVE;
}
static inline int rrp_local_span(uint64_t base, uint64_t size, uint64_t ptr, uint64_t bytes, uint64_t offset) {
    return bytes && ptr >= base && ptr-base <= size && bytes <= size-(ptr-base) &&
           (RRP_MUTANT == 3 || offset == ptr-base) && offset <= UINT32_MAX && bytes-1 <= UINT32_MAX-offset;
}
enum { RRP_END, RRP_WRITE, RRP_REPORT_FENCE, RRP_COPY_FENCE, RRP_COPY_OLD,
       RRP_COPY_NEW, RRP_COPY_BAD, RRP_PUBLISH, RRP_EARLY, RRP_GATE };
static inline const unsigned char *rrp_plan(unsigned claim, unsigned control) {
    static const unsigned char plans[4][2][8] = {
        {{RRP_WRITE, RRP_REPORT_FENCE, RRP_COPY_OLD, RRP_PUBLISH, RRP_END},
         {RRP_COPY_OLD, RRP_COPY_FENCE, RRP_WRITE, RRP_REPORT_FENCE, RRP_PUBLISH, RRP_END}},
        {{RRP_COPY_OLD, RRP_COPY_FENCE, RRP_WRITE, RRP_REPORT_FENCE, RRP_COPY_NEW, RRP_PUBLISH, RRP_END},
         {RRP_WRITE, RRP_REPORT_FENCE, RRP_COPY_OLD, RRP_COPY_NEW, RRP_PUBLISH, RRP_END}},
        {{RRP_COPY_OLD, RRP_PUBLISH, RRP_END}, {RRP_COPY_BAD, RRP_PUBLISH, RRP_END}},
        {{RRP_COPY_OLD, RRP_PUBLISH, RRP_END}, {RRP_EARLY, RRP_GATE, RRP_COPY_OLD, RRP_END}}
    };
    return plans[claim][control];
}
#endif
