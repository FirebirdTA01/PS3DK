/* cell/spurs/lv2_event_queue.h - LV2 event-queue attachment for SPURS.
 *
 * Lets a CellSpurs instance receive notifications via an LV2
 * sys_event_queue_t.  Caller creates a local queue, then attaches
 * it to the SPURS instance with cellSpursAttachLv2EventQueue.
 */
#ifndef __PS3DK_CELL_SPURS_LV2_EVENT_QUEUE_H__
#define __PS3DK_CELL_SPURS_LV2_EVENT_QUEUE_H__

#include <stdint.h>
#include <stdbool.h>
#include <sys/event.h>

/* Forward declaration from cell/spurs/types.h */
typedef struct CellSpurs CellSpurs;

#ifdef __cplusplus
extern "C" {
#endif

int cellSpursAttachLv2EventQueue(CellSpurs *spurs,
                                 sys_event_queue_t queue,
                                 uint8_t *port,
                                 bool isDynamic);

int cellSpursDetachLv2EventQueue(CellSpurs *spurs, uint8_t port);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* __PS3DK_CELL_SPURS_LV2_EVENT_QUEUE_H__ */
