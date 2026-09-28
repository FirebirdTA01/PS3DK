/* spurs-suite custom policy module (row spurs-module): the box the module
 * updates in main memory each time the kernel runs it. */
#ifndef SPURS_SUITE_MODULE_H
#define SPURS_SUITE_MODULE_H

#include <stdint.h>

#define M_MAGIC 0x504d0001u

typedef struct module_box {
    uint32_t magic;         /* M_MAGIC once the module has run */
    uint32_t runs;          /* times the kernel entered the module */
    uint32_t wid;           /* the module's own workload id */
    uint32_t spu;           /* the SPU it last ran on */
    uint32_t arg;           /* low word of the workload argument it was given */
    uint32_t pad[27];
} __attribute__((aligned(128))) module_box;

#endif
