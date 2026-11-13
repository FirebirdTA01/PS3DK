#ifndef PS3DK_PROF_AUTO_COMPLETION_H
#define PS3DK_PROF_AUTO_COMPLETION_H
#include <stdint.h>

#define auto_publication_commit ps3dk_prof__auto_publication_commit
#define auto_publication_before ps3dk_prof__auto_publication_before
#define auto_publication_after ps3dk_prof__auto_publication_after
#define auto_publication_read ps3dk_prof__auto_publication_read
#define auto_completion_read ps3dk_prof__auto_completion_read

/* Single emitting owner, single drainer. Metadata commit precedes committed;
 * before snapshots it before the actual PUT; after release-publishes that exact
 * snapshot after PUT's barrier. Advancing committed alone is not publication. */
typedef struct auto_publication { uint32_t committed, published; } auto_publication;
int auto_publication_commit(auto_publication *,uint32_t serial);
uint32_t auto_publication_before(const auto_publication *);
int auto_publication_after(auto_publication *,uint32_t snapshot);
uint32_t auto_publication_read(const auto_publication *);

typedef struct auto_completion { uint32_t last_label, invalid; } auto_completion;
typedef struct auto_completion_port {
    void *user;
    uint32_t (*label)(void *);
    uint64_t (*timer)(void *,uint32_t slot);
    void (*sync)(void *);
} auto_completion_port;
typedef struct auto_completion_fact {
    uint64_t raw_begin, raw_end;
    uint32_t published;
} auto_completion_fact;
enum { AUTO_COMPLETION_PENDING=0, AUTO_COMPLETION_READY=1,
       AUTO_COMPLETION_DEFER=2, AUTO_COMPLETION_REFUSED=3 };

/* Drainer only. Caller retains both physical slots until whole sink acceptance.
 * READY depends on the declared report-before-label/coherent-read premises;
 * this code does not prove them or the initial clean-context handoff. It never
 * frees resources, resets labels, infers an epoch or retries a device read.
 * A label ahead of the acquired PUT frontier is DEFER: a subsequent invocation
 * takes a fresh acquire snapshot. Regression is sticky, including reset to 0. */
int auto_completion_read(auto_completion *,const auto_publication *,const auto_completion_port *,
                         uint32_t begin_slot,uint32_t end_slot,uint32_t begin_serial,uint32_t end_serial,
                         auto_completion_fact *);
#endif
