/*
 * hello-ppu-sys-event — Sony-named LV2 event-queue/event-port surface.
 *
 * Exercises the snake_case wrappers our <sys/event.h> umbrella adds
 * over PSL1GHT's camelCase sysEvent* surface:
 *
 *   sys_event_queue_attribute_initialize  (macro form, takes-by-value)
 *   sys_event_queue_create / destroy / receive / drain
 *   sys_event_port_create / destroy / connect_local / send
 *
 * Plus the SYS_EVENT_QUEUE_LOCAL / SYS_PPU_QUEUE / SYS_SYNC_PRIORITY
 * constants we define for source-compat.
 *
 * Flow:
 *   1. Build a queue, connect a port, send three messages.
 *   2. Drain the queue, count received events (expect 3).
 *   3. Tear down; report PASS/FAIL.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/process.h>
#include <sys/event.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define IPC_KEY    0x1234567811223344ULL
#define EVENT_NAME 0xaabbccddee001122ULL

int main(void)
{
	printf("hello-ppu-sys-event: Sony snake_case LV2 event surface\n");

	sys_event_queue_attribute_t attr;
	sys_event_queue_attribute_initialize(attr);
	memcpy(attr.name, "ps3tcQ\0", 7);
	printf("  attr.attr_protocol=%d  type=%d  name=%.7s\n",
	       (int)attr.attr_protocol, (int)attr.type, attr.name);

	sys_event_queue_t q = 0;
	int rc = sys_event_queue_create(&q, &attr, IPC_KEY, 16);
	printf("  sys_event_queue_create -> rc=0x%08x  q=0x%08x\n",
	       (unsigned)rc, (unsigned)q);
	if (rc != 0) return 1;

	sys_event_port_t port = 0;
	rc = sys_event_port_create(&port, SYS_EVENT_PORT_LOCAL, EVENT_NAME);
	printf("  sys_event_port_create -> rc=0x%08x  port=0x%08x\n",
	       (unsigned)rc, (unsigned)port);

	rc = sys_event_port_connect_local(port, q);
	printf("  sys_event_port_connect_local -> rc=0x%08x\n", (unsigned)rc);

	for (int i = 1; i <= 3; i++) {
		rc = sys_event_port_send(port, (uint64_t)i, (uint64_t)(i * 10), 0xdead0000 + i);
		printf("  sys_event_port_send(%d) -> rc=0x%08x\n", i, (unsigned)rc);
	}

	int got = 0;
	for (int i = 0; i < 3; i++) {
		sys_event_t ev;
		memset(&ev, 0, sizeof(ev));
		rc = sys_event_queue_receive(q, &ev, 1000 * 1000);
		if (rc == 0) {
			printf("  recv[%d]: source=0x%016llx  d1=%llu  d2=%llu  d3=0x%llx\n",
			       i,
			       (unsigned long long)ev.source,
			       (unsigned long long)ev.data_1,
			       (unsigned long long)ev.data_2,
			       (unsigned long long)ev.data_3);
			got++;
		} else {
			printf("  recv[%d] -> rc=0x%08x (timeout/error)\n", i, (unsigned)rc);
			break;
		}
	}

	rc = sys_event_port_disconnect(port);
	printf("  sys_event_port_disconnect -> rc=0x%08x\n", (unsigned)rc);

	rc = sys_event_port_destroy(port);
	printf("  sys_event_port_destroy -> rc=0x%08x\n", (unsigned)rc);

	rc = sys_event_queue_destroy(q, 0);
	printf("  sys_event_queue_destroy -> rc=0x%08x\n", (unsigned)rc);

	printf("  result: %d/3 events delivered — %s\n",
	       got, got == 3 ? "PASS" : "FAIL");
	return got == 3 ? 0 : 1;
}
