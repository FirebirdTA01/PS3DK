#ifndef HETEROGENEOUS_LIVE_H
#define HETEROGENEOUS_LIVE_H
#include <stddef.h>
#define SAMPLE_LIVE_PORT 31750u
/* After profiler init: the network, then the live link. Fills status with what the screen shows (where to connect,
 * or why live is off); a failure never stops the run. Every network and live result, from here to sample_live_end,
 * is also recorded in <csv path without ".csv">.live.txt, which this module owns. */
void sample_live_begin(const char *csv_path, char *status, size_t n);
/* After profiler shutdown, so a connected viewer can receive END: stop the live link, then the network, by result;
 * records each attempt and closes the live record. Never changes the workload result. */
void sample_live_end(void);
#endif
