#ifndef MODULE_EXPORT_CONTRACT_H
#define MODULE_EXPORT_CONTRACT_H

#include <stddef.h>
#include <stdint.h>

#define SAMPLE_MODULE_MAGIC 0x53505258 /* 'SPRX' */
#define SAMPLE_MODULE_NAME  "sample_module_export"

typedef struct sample_module_handoff {
    uint32_t magic;
    uint32_t start_counter;
    int (*add)(int a, int b);
} sample_module_handoff_t;

int sample_calc_add(int a, int b);
uint32_t sample_get_start_counter(void);

#endif /* MODULE_EXPORT_CONTRACT_H */
