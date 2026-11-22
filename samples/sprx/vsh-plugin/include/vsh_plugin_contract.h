#ifndef VSH_PLUGIN_CONTRACT_H
#define VSH_PLUGIN_CONTRACT_H

/*
 * Shared between the plugin (module/plugin.c) and the loader (loader/main.c).
 * Every field is a uint32_t so the layout is the same under ILP32 and LP64.
 * Only the loader path passes these reports: a plugin started by a VSH
 * plugin loader gets no argp.
 */

#include <stdint.h>

/* The log the plugin's worker writes, one line at a time, each flushed to
 * disk with fsync so the last line survives a freeze. */
#define VSHP_LOG_PATH "/dev_hdd0/tmp/ps3dk-vsh-plugin.log"

#define VSHP_START_MAGIC 0x56534831u /* 'VSH1' */
#define VSHP_STOP_MAGIC  0x56534832u /* 'VSH2' */

/* The .data counter starts here; each worker tick adds one to it and to a
 * .bss counter that starts at zero. */
#define VSHP_DATA_INITIAL 0x0DA70000u

/* How module_start / module_stop were reached. */
#define VSHP_CALLER_LOADER 1u   /* sys_prx_start_module / sys_prx_stop_module */
#define VSHP_CALLER_BARE   2u   /* a thread with no caller (a VSH plugin loader) */

/* Filled in by module_start through argp. */
typedef struct vshp_start_report {
	uint32_t magic;          /* VSHP_START_MAGIC once module_start has run   */
	uint32_t caller;         /* VSHP_CALLER_*                                 */
	uint32_t create_rc;      /* sys_ppu_thread_create's return code           */
	uint32_t data_initial;   /* the .data counter as loaded                   */
} vshp_start_report_t;

/* Filled in by module_stop through argp. */
typedef struct vshp_stop_report {
	uint32_t magic;          /* VSHP_STOP_MAGIC once module_stop has run      */
	uint32_t caller;         /* VSHP_CALLER_*                                 */
	uint32_t join_rc;        /* sys_ppu_thread_join's return code             */
	uint32_t worker_exit;    /* the worker's exit code: its tick count        */
	uint32_t ticks_data;     /* the .data counter after the worker stopped    */
	uint32_t ticks_bss;      /* the .bss counter after the worker stopped     */
	uint32_t log_failures;   /* log open/write/fsync calls that failed        */
} vshp_stop_report_t;

#endif /* VSH_PLUGIN_CONTRACT_H */
