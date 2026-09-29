/* spurs-suite custom policy module (row spurs-module): the box the module
 * updates in main memory each time the kernel runs it. */
#ifndef SPURS_SUITE_MODULE_H
#define SPURS_SUITE_MODULE_H

#include <stdint.h>

#define M_MAGIC 0x504d0001u
#define M_UNIT_BASE 0x20000u     /* where the module loads the work unit */
#define M_UNIT_OK   0x226cu
#define M_TRACE_MAGIC 0x504d7ace   /* payload high word of the module trace packet */

typedef struct module_box {
    uint32_t magic;         /* M_MAGIC once the module has run */
    uint32_t runs;          /* times the kernel entered the module */
    uint32_t wid;           /* the module's own workload id */
    uint32_t spu;           /* the SPU it last ran on */
    uint32_t arg;           /* low word of the workload argument it was given */
    uint32_t unitEa;        /* in: a -mcustom-module work unit image to run, or 0 */
    uint32_t unitSize;      /* in: its size in bytes */
    uint32_t unitResult;    /* out: what the unit returned */
    uint32_t pad[24];
} __attribute__((aligned(128))) module_box;

#endif
