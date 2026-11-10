#ifndef PS3DK_PROF_AUTO_CONFIG_H
#define PS3DK_PROF_AUTO_CONFIG_H
#include "auto_wire.h"

#define auto_config_init ps3dk_prof__auto_config_init
#define auto_config_begin ps3dk_prof__auto_config_begin
#define auto_config_commit ps3dk_prof__auto_config_commit
#define auto_config_abort ps3dk_prof__auto_config_abort
#define auto_config_freeze ps3dk_prof__auto_config_freeze
#define auto_config_stop ps3dk_prof__auto_config_stop
#define auto_config_invalidate ps3dk_prof__auto_config_invalidate
#define auto_config_flags ps3dk_prof__auto_config_flags
#define auto_config_read ps3dk_prof__auto_config_read
#define auto_config_can_admit ps3dk_prof__auto_config_can_admit

typedef struct auto_config_store {
    uint32_t state, flags;
    auto_wire_config config;
} auto_config_store;

/* init is capture setup, with no concurrent access. The initial immutable
 * identity describes NO_GCM_INIT_OBSERVED until an intercepted init updates it.
 * Resource validation, reservation and cleanup belong to the runtime registry;
 * this component never touches device state or assumes device completion. */
void auto_config_init(auto_config_store *, uint64_t frequency, const uint8_t mechanism[32],
                      const uint8_t binding[32]);
/* A nonzero ticket owns the mutable CONFIG until exactly one commit/abort on
 * that same configuring thread. Neither observation nor a drain may read it
 * meanwhile. Invalid overrides abort, preserving the previous default lease.
 * Only already validated candidate resources may be committed; a false return
 * means the caller must unwind its unpublished resources. No marker may be
 * issued during this claim. */
uint32_t auto_config_begin(auto_config_store *);
int auto_config_commit(auto_config_store *, uint32_t ticket, const auto_wire_config *);
void auto_config_abort(auto_config_store *, uint32_t ticket);
/* freeze is an OWNER observation (operation/clock/frame), never a routine
 * background drain. After it succeeds CONFIG is immutable for the capture.
 * stop is the seal's claim, before quiescence or sink checks; a writer in flight
 * retains the bytes until its commit/abort publishes the frozen outcome. */
int auto_config_freeze(auto_config_store *);
void auto_config_stop(auto_config_store *);
void auto_config_invalidate(auto_config_store *, uint32_t status_flags);
uint32_t auto_config_flags(const auto_config_store *);
const auto_wire_config *auto_config_read(const auto_config_store *);
int auto_config_can_admit(const auto_config_store *);
#endif
