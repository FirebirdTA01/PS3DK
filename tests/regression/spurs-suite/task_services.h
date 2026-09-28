/* spurs-suite task services: task kinds and result slot numbers. */
#ifndef SPURS_SUITE_TASK_SERVICES_H
#define SPURS_SUITE_TASK_SERVICES_H

#define TS_GETTERS          0
#define TS_YIELD            1
#define TS_SIGNAL_SELF      2
#define TS_WAIT_SIGNAL      3
#define TS_SEM_CONSUMER     4
#define TS_SEM_PRODUCER     5
/* extra slots written along the way */
#define TS_GETTERS_TASKSET  6
#define TS_GETTERS_SPURS    7
#define TS_WAIT_SIGNAL_READY 8
#define TS_SLOTS            9

#define TS_SEM_ROUNDS       8

#endif
