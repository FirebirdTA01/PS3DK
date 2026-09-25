/*! \file sys/event.h
 \brief Reference-SDK source-compat umbrella for the LV2 event-queue
        / event-flag / event-port / condition-variable surface.

  Sony's <sys/event.h> bundles the entire LV2 sync syscall surface
  in one header.  Our PSL1GHT-shape SDK splits the same calls across
  <sys/event_queue.h>, <sys/cond.h>, <sys/sem.h>, <sys/mutex.h>, and
  <sys/synchronization.h>; PSL1GHT also names the entry points camelCase
  (sysEventQueueCreate) instead of Sony's snake_case
  (sys_event_queue_create).  This umbrella:

    1. Pulls in the split headers so the entire surface is reachable
       through a single <sys/event.h>.
    2. Aliases the Sony-named struct types (sys_event_queue_attribute_t)
       onto our PSL1GHT-named ones (sys_event_queue_attr_t).
    3. Provides static-inline forwarders for the snake_case Sony names.
    4. Defines the SYS_EVENT_QUEUE_LOCAL / SYS_EVENT_PORT_LOCAL
       enumerator names the reference SDK uses.

  Source written against Sony's monolithic <sys/event.h> compiles
  unchanged against this umbrella.
*/

#ifndef PS3TC_SYS_EVENT_H
#define PS3TC_SYS_EVENT_H

#include <errno.h>
/* lv2 syscalls return the kernel status CELL_EBUSY (0x8001000A), not the libc
 * POSIX EBUSY (16) that <errno.h> defines above.  Restore the lv2 value so
 * source-compatible callers that test a syscall result against EBUSY (e.g. the
 * FW flip handlers queue-full tolerance) compare correctly; other POSIX errno
 * names stay available for libc-style use. */
#undef EBUSY
#define EBUSY (-2147418102) /* 0x8001000A, lv2 CELL_EBUSY */
#include <sys/return_code.h>
#include <sys/event_queue.h>
#include <sys/cond.h>
#include <sys/sem.h>
#include <sys/mutex.h>
#include <sys/synchronization.h>
#include <sys/process.h>

#ifdef __cplusplus
extern "C" {
#endif



/* sys_event_queue_attribute_initialize: macro form matching the
 * reference SDK's <sys/event.h> shape — takes the struct by value
 * and sets default attr_protocol = SYS_SYNC_PRIORITY, type = SYS_PPU_QUEUE,
 * name = "". */
#ifndef SYS_SYNC_PRIORITY
# define SYS_SYNC_PRIORITY  1
#endif
#ifndef SYS_PPU_QUEUE
# define SYS_PPU_QUEUE      SYS_EVENT_QUEUE_PPU
#endif
#ifndef SYS_SPU_QUEUE
# define SYS_SPU_QUEUE      SYS_EVENT_QUEUE_SPU
#endif

#define sys_event_queue_attribute_initialize(x)        \
    do {                                               \
        (x).attr_protocol = SYS_SYNC_PRIORITY;         \
        (x).type          = SYS_PPU_QUEUE;             \
        (x).name[0]       = '\0';                      \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_SYS_EVENT_H */
