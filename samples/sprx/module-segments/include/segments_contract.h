#ifndef SEGMENTS_CONTRACT_H
#define SEGMENTS_CONTRACT_H

/*
 * Shared between the module (module/module.c) and the loader (loader/main.c).
 * Every field is a uint32_t so the layout is the same under ILP32 and LP64.
 */

#include <stdint.h>

#define SEG_START_MAGIC  0x53454753u /* 'SEGS' */
#define SEG_STOP_MAGIC   0x53454754u /* 'SEGT' */

/* Initial value of the module's .data global, and what start/stop write. */
#define SEG_DATA_INITIAL 0x0DA7A001u
#define SEG_DATA_STARTED 0x5EED0001u
#define SEG_DATA_STOPPED 0x5EED0003u
/* What start writes to the module's .bss global (which begins at zero). */
#define SEG_BSS_STARTED  0x5EED0002u

#define SEG_BSS_BLOCK_WORDS 64u

/* Filled in by module_start through argp. */
typedef struct seg_start_report {
	uint32_t magic;        /* SEG_START_MAGIC once module_start has run     */
	uint32_t data_before;  /* .data global as loaded (SEG_DATA_INITIAL)     */
	uint32_t data_after;   /* read back after writing SEG_DATA_STARTED      */
	uint32_t bss_before;   /* .bss global as loaded (0)                     */
	uint32_t bss_after;    /* read back after writing SEG_BSS_STARTED       */
	uint32_t block_bad;    /* words of a .bss block that failed read-back   */
	uint32_t rodata_sum;   /* sum of a .rodata table (SEG_RODATA_SUM)       */
} seg_start_report_t;

/* Filled in by module_stop through argp. */
typedef struct seg_stop_report {
	uint32_t magic;        /* SEG_STOP_MAGIC once module_stop has run       */
	uint32_t data_before;  /* .data global at stop (SEG_DATA_STARTED)       */
	uint32_t data_after;   /* read back after writing SEG_DATA_STOPPED      */
	uint32_t bumps;        /* how often seg_state_bump ran                  */
} seg_stop_report_t;

/* The .rodata table the module sums: 1 + 2 + ... + 8. */
#define SEG_RODATA_SUM 36u

/* Library "seg_calc" */
int seg_calc_add(int a, int b);        /* a + b + 0x42                     */
int seg_calc_scale(int x, int i);      /* x * k_table[i & 7], .rodata read */

/* Library "seg_state" */
uint32_t seg_state_data(void);         /* the .data global                 */
uint32_t seg_state_bss(void);          /* the .bss global                  */
uint32_t seg_state_bump(void);         /* ++ a .bss counter, returns it    */

#endif /* SEGMENTS_CONTRACT_H */
