#ifndef PS3DK_PROF_AUTO_LEASE_H
#define PS3DK_PROF_AUTO_LEASE_H
#include <stdint.h>
#include "../pal/pal.h"
struct prof;

/* Claim/drop and their fields require the existing extension mutex, shared with
 * manual GPU attachment and calibration. No device access is performed here.
 * Raw/private command writers remain outside this managed ownership registry. */
typedef struct auto_resource_lease {
    void *ctx;
    uint32_t domain, first, count, operation_label, clock_label;
    uint32_t ticket, next_ticket, published;
    uint32_t owner_incarnation, owner_generation;
    pal_owner owner;
} auto_resource_lease;

#define p_auto_lease_claim ps3dk_prof__auto_lease_claim
#define p_auto_lease_drop ps3dk_prof__auto_lease_drop
#define p_auto_lease_mark ps3dk_prof__auto_lease_mark
#define p_auto_lease_owner ps3dk_prof__auto_lease_owner

/* Caller owns the mutable CONFIG claim and has already validated its owner and
 * prepared allocation/mappings. Invalid replacement leaves the old lease intact.
 * The id frontier is shared with manual GPU domains and never rolled back.
 * The ticket prevents cleanup of an old candidate from removing its replacement.
 * Seal racing this claim is decided by CONFIG commit; before any device writes
 * a losing owner must drop its exact, unpublished ticket under this same lock. */
int p_auto_lease_claim(struct prof *, void *, uint32_t first, uint32_t count,
                      uint32_t operation_label, uint32_t clock_label,
                      uint32_t *domain, uint32_t *ticket);
/* mark is also allowed on the sole emitting owner after CONFIG is frozen:
 * at that point claim/drop cannot run and all identity fields are immutable.
 * published is atomic for both calling disciplines. mark immediately precedes
 * the first device write. Afterwards the reservation
 * remains held through process exit; a logical reset is not GPU retirement. */
int p_auto_lease_mark(struct prof *, uint32_t ticket);
int p_auto_lease_drop(struct prof *, uint32_t ticket);
/* Takes/releases the extension lock. ENOTSUP means this is not the AUTO domain;
 * all other results describe that domain, including owner/context refusal. */
int p_auto_lease_owner(struct prof *, uint32_t domain, void *ctx);
#endif
