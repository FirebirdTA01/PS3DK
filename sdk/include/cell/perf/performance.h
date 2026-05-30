/*
 * cell/perf/performance.h
 *
 * Cell Performance Monitor API
 *
 * Provides access to the Cell Broadband Engine hardware performance
 * monitoring counters, trace buffers, and LV2 OS trace facilities.
 */

#ifndef __PS3DK_CELL_PERF_PERFORMANCE_H__
#define __PS3DK_CELL_PERF_PERFORMANCE_H__

#include <stddef.h>
#include <ppu-types.h>
#include <sys/spu_thread.h>
#include <cell/error.h>

/* ------------------------------------------------------------------ *
 * Buffer / counter sizing
 * ------------------------------------------------------------------ */

#define CELL_PERF_CBEPM_TRACE_CACHE_BUFFER_SIZE     0x00001000
#define CELL_PERF_CBEPM_EXTERNAL_TRACE_BUFFER_SIZE  0x20000000
#define CELL_PERF_CBEPM_INTERNAL_TRACE_BUFFER_SIZE  0x00004000
#define CELL_PERF_CBEPM_TRACE_DATA_SIZE             16
#define CELL_PERF_CBEPM_NUM_32BIT_COUNTERS          4
#define CELL_PERF_CBEPM_NUM_16BIT_COUNTERS          8
#define CELL_PERF_CBEPM_NUM_8BIT_COUNTERS           8
#define CELL_PERF_CBEPM_PPU_BOOKMARK_MASK           0x3fffffffffffffffULL

#define CELL_PERF_TRACE_ARRAY_DEPTH                 1024
#define CELL_PERF_TRACE_DATA_WIDTH                  16
#define CELL_PERF_TRACE_ARRAY_SIZE                  \
	(CELL_PERF_TRACE_ARRAY_DEPTH * CELL_PERF_TRACE_DATA_WIDTH)
#define CELL_PERF_NUM_16BIT_COUNTERS                8
#define CELL_PERF_NUM_32BIT_COUNTERS                \
	(CELL_PERF_NUM_16BIT_COUNTERS / 2)
#define CELL_PERF_MAX_SPU_TARGET                    2

#define CELL_PERF_SIGNAL_SETUP_MASK                 0x01c00000
#define CELL_PERF_SIGNAL_SETUP_ENABLE               0x00400000
#define CELL_PERF_SIGNAL_SETUP_POSITIVE             0x01000000
#define CELL_PERF_SIGNAL_SETUP_CYCLE                0x00800000
#define CELL_PERF_SIGNAL_SETUP_DISABLE              0x00000000
#define CELL_PERF_MAX_THREAD_NAME_LENGTH            27
#define CELL_PERF_TRACE_DATA_CACHE_RECOMMENDED      \
	CELL_PERF_CBEPM_TRACE_CACHE_BUFFER_SIZE

/* ------------------------------------------------------------------ *
 * Setup configuration bits (CellPerfCBEpmSetup::conf /
 * CellPerfSetup::conf)
 * ------------------------------------------------------------------ */

#define CELL_PERF_SETUP_CONF_BUFFER_TYPE_NA         0x00000000
#define CELL_PERF_SETUP_CONF_BUFFER_TYPE_INTERNAL   0x00000001
#define CELL_PERF_SETUP_CONF_BUFFER_TYPE_EXTERNAL   0x00000002
#define CELL_PERF_SETUP_CONF_BUFFER_TYPE_MASK       0x00000007
#define CELL_PERF_SETUP_CONF_BUFFER_OVERWRITE       0x00000010
#define CELL_PERF_SETUP_CONF_COUNTER8               0x00000200
#define CELL_PERF_SETUP_CONF_COUNTER16              0x00000400
#define CELL_PERF_SETUP_CONF_COUNTER32              0x00000800
#define CELL_PERF_SETUP_CONF_PPU_BOOKMARK_EN        0x00002000
#define CELL_PERF_SETUP_CONF_SPU_ADDR_TRACE_EN_0    0x00010000
#define CELL_PERF_SETUP_CONF_SPU_ADDR_TRACE_EN_1    0x00020000
#define CELL_PERF_SETUP_CONF_SPU_BOOKMARK_EN_0      0x00040000
#define CELL_PERF_SETUP_CONF_SPU_BOOKMARK_EN_1      0x00080000
#define CELL_PERF_SETUP_CONF_SPU_TRIGGER_EN_0       0x00100000
#define CELL_PERF_SETUP_CONF_SPU_TRIGGER_EN_1       0x00200000
#define CELL_PERF_SETUP_CONF_VERBOSE_MODE           0x80000000
#define CELL_PERF_SETUP_CONF_SUPERVISOR_MODE        0x40000000
#define CELL_PERF_SETUP_CONF_NO_TRACE_MODE          0x20000000
#define CELL_PERF_SETUP_CONF_COUNTER_WRAP_MODE      0x10000000
#define CELL_PERF_SETUP_CONF_NO_TRIGGER             0x01000000
#define CELL_PERF_SETUP_CONF_NO_LV1_BOOKMARK        0x02000000

/* ------------------------------------------------------------------ *
 * Signal group identifiers
 * ------------------------------------------------------------------ */

#define CELL_PERF_SIGNAL_GROUP_PPU_IU_1             0x01
#define CELL_PERF_SIGNAL_GROUP_PPU_IU_2             0x02
#define CELL_PERF_SIGNAL_GROUP_PPU_XU               0x03
#define CELL_PERF_SIGNAL_GROUP_PPU_VMX_FPU          0x04
#define CELL_PERF_SIGNAL_GROUP_PPU_MMU              0x05
#define CELL_PERF_SIGNAL_GROUP_MFC_L2_A             0x0b
#define CELL_PERF_SIGNAL_GROUP_MFC_L2_B             0x0c
#define CELL_PERF_SIGNAL_GROUP_MFC_L2_C             0x0d
#define CELL_PERF_SIGNAL_GROUP_MFC_L2_D             0x0e
#define CELL_PERF_SIGNAL_GROUP_MFC_NCU              0x0f
#define CELL_PERF_SIGNAL_GROUP_SPU_MON              0x10
#define CELL_PERF_SIGNAL_GROUP_SMF_ATO              0x13
#define CELL_PERF_SIGNAL_GROUP_SMF_DMA_1            0x14
#define CELL_PERF_SIGNAL_GROUP_SMF_DMA_2            0x15
#define CELL_PERF_SIGNAL_GROUP_SMF_SBI_1            0x16
#define CELL_PERF_SIGNAL_GROUP_EIB_DARB_1           0x1c
#define CELL_PERF_SIGNAL_GROUP_EIB_DARB_2           0x1d
#define CELL_PERF_SIGNAL_GROUP_MIC_0                0x32
#define CELL_PERF_SIGNAL_GROUP_MIC_1                0x33
#define CELL_PERF_SIGNAL_GROUP_BIC_IOIF0_0          0x35
#define CELL_PERF_SIGNAL_GROUP_BIC_IOIF1_0          0x36
#define CELL_PERF_SIGNAL_GROUP_BIC_IOIF0_1          0x37
#define CELL_PERF_SIGNAL_GROUP_BIC_IOIF1_1          0x38
#define CELL_PERF_SIGNAL_GROUP_IOC_1                0x39
#define CELL_PERF_SIGNAL_GROUP_IOC_2                0x3a

/* ------------------------------------------------------------------ *
 * Individual signal names
 *
 * The high byte encodes the signal group; the low byte selects the
 * specific event within that group.
 * ------------------------------------------------------------------ */

/* PPU IU_1 — thread 0 */
#define CELL_PERF_SIGNAL_PPU_TH0_BRANCH_INSTRUCTION_COMPLETED                  0x0100
#define CELL_PERF_SIGNAL_PPU_TH0_BRANCH_MISPREDICTION                          0x0101
#define CELL_PERF_SIGNAL_PPU_TH0_INSTRUCTION_BUFFER_EMPTY                      0x0102
#define CELL_PERF_SIGNAL_PPU_TH0_INSTRUCTION_ERAT_MISS                         0x0103
#define CELL_PERF_SIGNAL_PPU_TH0_L1_ICACHE_MISS                                0x0104
#define CELL_PERF_SIGNAL_PPU_TH0_LAST_MICROCODE                                0x0105
#define CELL_PERF_SIGNAL_PPU_TH0_DISPATCH_BLOCKED                              0x0106
#define CELL_PERF_SIGNAL_PPU_TH0_ISSUE_STALL                                   0x0107
#define CELL_PERF_SIGNAL_PPU_TH0_ISSUE_STALL_DUE_TO_REG_DEPENDENCY             0x0108
#define CELL_PERF_SIGNAL_PPU_TH0_INSTRUCTION_FLUSH                             0x0109
#define CELL_PERF_SIGNAL_PPU_TH0_TWO_INSTRUCTIONS_ISSUED_AT_SAME_TIME          0x010a
#define CELL_PERF_SIGNAL_PPU_TH0_TWO_PPC_INSTRUCTIONS_COMPLETED                0x010b
#define CELL_PERF_SIGNAL_PPU_TH0_EXTERNAL_INTERRUPT_PENDING_TOTAL              0x010c
#define CELL_PERF_SIGNAL_PPU_TH0_EXTERNAL_INTERRUPT_PENDING_HARDWARE           0x010d
#define CELL_PERF_SIGNAL_PPU_TH0_TWO_INTEGER_INSTRUCTIONS_COMPLETED            0x010e
#define CELL_PERF_SIGNAL_PPU_TH0_TWO_FLOATING_INSTRUCTIONS_COMPLETED           0x010f
#define CELL_PERF_SIGNAL_PPU_TH0_STALL_DUE_TO_ISYNC                            0x0110
#define CELL_PERF_SIGNAL_PPU_TH0_VIQ_STALL                                     0x0111
#define CELL_PERF_SIGNAL_PPU_TH0_VIQ_STALL_DUE_TO_REG_DEPENDENCY               0x0112

/* PPU IU_1 — thread 1 */
#define CELL_PERF_SIGNAL_PPU_TH1_BRANCH_INSTRUCTION_COMPLETED                  0x0113
#define CELL_PERF_SIGNAL_PPU_TH1_BRANCH_MISPREDICTION                          0x0114
#define CELL_PERF_SIGNAL_PPU_TH1_INSTRUCTION_BUFFER_EMPTY                      0x0115
#define CELL_PERF_SIGNAL_PPU_TH1_INSTRUCTION_ERAT_MISS                         0x0116
#define CELL_PERF_SIGNAL_PPU_TH1_L1_ICACHE_MISS                                0x0117
#define CELL_PERF_SIGNAL_PPU_TH1_LAST_MICROCODE                                0x0118
#define CELL_PERF_SIGNAL_PPU_TH1_DISPATCH_BLOCKED                              0x0119
#define CELL_PERF_SIGNAL_PPU_TH1_ISSUE_STALL                                   0x011a
#define CELL_PERF_SIGNAL_PPU_TH1_ISSUE_STALL_DUE_TO_REG_DEPENDENCY             0x011b
#define CELL_PERF_SIGNAL_PPU_TH1_INSTRUCTION_FLUSH                             0x011c
#define CELL_PERF_SIGNAL_PPU_TH1_TWO_INSTRUCTIONS_ISSUED_AT_SAME_TIME          0x011d
#define CELL_PERF_SIGNAL_PPU_TH1_TWO_PPC_INSTRUCTIONS_COMPLETED                0x011e
#define CELL_PERF_SIGNAL_PPU_TH1_EXTERNAL_INTERRUPT_PENDING_TOTAL              0x011f
#define CELL_PERF_SIGNAL_PPU_TH1_EXTERNAL_INTERRUPT_PENDING_HARDWARE           0x0200
#define CELL_PERF_SIGNAL_PPU_TH1_TWO_INTEGER_INSTRUCTIONS_COMPLETED            0x0201
#define CELL_PERF_SIGNAL_PPU_TH1_TWO_FLOATING_INSTRUCTIONS_COMPLETED           0x0202
#define CELL_PERF_SIGNAL_PPU_TH1_STALL_DUE_TO_ISYNC                            0x0203
#define CELL_PERF_SIGNAL_PPU_TH1_VIQ_STALL                                     0x0204
#define CELL_PERF_SIGNAL_PPU_TH1_VIQ_STALL_DUE_TO_REG_DEPENDENCY               0x0205

/* PPU XU — thread 0 */
#define CELL_PERF_SIGNAL_PPU_TH0_MFSPR_STALL                                   0x0300
#define CELL_PERF_SIGNAL_PPU_TH0_INTEGER_MUL_OR_DIV_STALL                      0x0301
#define CELL_PERF_SIGNAL_PPU_TH0_DATA_ERAT_MISS                                0x0302
#define CELL_PERF_SIGNAL_PPU_TH0_LOAD_VALID                                    0x0304
#define CELL_PERF_SIGNAL_PPU_TH0_L1_DCACHE_MISS                                0x0305
#define CELL_PERF_SIGNAL_PPU_TH0_FLUSH_DUE_TO_MISALIGNED                       0x0306
#define CELL_PERF_SIGNAL_PPU_TH0_FLUSH_DUE_TO_QUEUE_FULL                       0x0307
#define CELL_PERF_SIGNAL_PPU_TH0_FLUSH_DUE_TO_STORE_QUEUE_FULL                 0x0307
#define CELL_PERF_SIGNAL_PPU_TH0_STORE_CONDITIONAL_WAIT                        0x0308
#define CELL_PERF_SIGNAL_PPU_TH0_HEAVYWEIGHT_SYNC                              0x0309
#define CELL_PERF_SIGNAL_PPU_TH0_STORE_CONDITIONAL_FAILED                      0x030a
#define CELL_PERF_SIGNAL_PPU_TH0_LOAD_HIT_STORE                                0x030b
#define CELL_PERF_SIGNAL_PPU_TH0_LOAD_HIT_RELOAD                               0x030c
#define CELL_PERF_SIGNAL_PPU_TH0_LOAD_HIT_RELOAD_WITH_TOUCH                    0x030d
#define CELL_PERF_SIGNAL_PPU_TH0_TOUCH_REQUEST_TO_L2                           0x030e
#define CELL_PERF_SIGNAL_PPU_TH0_TOUCH_HIT_L1_CACHE                            0x030f

/* PPU XU — thread 1 */
#define CELL_PERF_SIGNAL_PPU_TH1_MFSPR_STALL                                   0x0310
#define CELL_PERF_SIGNAL_PPU_TH1_INTEGER_MUL_OR_DIV_STALL                      0x0311
#define CELL_PERF_SIGNAL_PPU_TH1_DATA_ERAT_MISS                                0x0312
#define CELL_PERF_SIGNAL_PPU_TH1_LOAD_VALID                                    0x0314
#define CELL_PERF_SIGNAL_PPU_TH1_L1_DCACHE_MISS                                0x0315
#define CELL_PERF_SIGNAL_PPU_TH1_FLUSH_DUE_TO_MISALIGNED                       0x0316
#define CELL_PERF_SIGNAL_PPU_TH1_FLUSH_DUE_TO_QUEUE_FULL                       0x0317
#define CELL_PERF_SIGNAL_PPU_TH1_FLUSH_DUE_TO_STORE_QUEUE_FULL                 0x0317
#define CELL_PERF_SIGNAL_PPU_TH1_STORE_CONDITIONAL_WAIT                        0x0318
#define CELL_PERF_SIGNAL_PPU_TH1_HEAVYWEIGHT_SYNC                              0x0319
#define CELL_PERF_SIGNAL_PPU_TH1_STORE_CONDITILNAL_FAILED                      0x031a
#define CELL_PERF_SIGNAL_PPU_TH1_LOAD_HIT_STORE                                0x031b
#define CELL_PERF_SIGNAL_PPU_TH1_LOAD_HIT_RELOAD                               0x031c
#define CELL_PERF_SIGNAL_PPU_TH1_LOAD_HIT_RELOAD_WITH_TOUCH                    0x031d
#define CELL_PERF_SIGNAL_PPU_TH1_TOUCH_REQUEST_TO_L2                           0x031e
#define CELL_PERF_SIGNAL_PPU_TH1_TOUCH_HIT_L1_CACHE                            0x031f

/* PPU VMX/FPU — thread 0 */
#define CELL_PERF_SIGNAL_PPU_TH0_TWO_VMX_OR_SCALAR_INSTRUCTION_COMPLETED       0x0400
#define CELL_PERF_SIGNAL_PPU_TH0_SLOT0_INSTRUCTION_COMPLETED                   0x0401
#define CELL_PERF_SIGNAL_PPU_TH0_SLOT1_INSTRUCTION_COMPLETED                   0x0402
#define CELL_PERF_SIGNAL_PPU_TH0_VMX_SIMPLE_PIPELINE_INSTRUCTION_COMPLETED     0x0403
#define CELL_PERF_SIGNAL_PPU_TH0_VMX_FLOAT_PIPELINE_INSTRUCTION_COMPLETED      0x0404
#define CELL_PERF_SIGNAL_PPU_TH0_RECIPROCAL_ESTIMATE_INSTRUCTION_COMPLETED     0x0405
#define CELL_PERF_SIGNAL_PPU_TH0_VMX_PERMUTE_PIPELINE_INSTRUCTION_COMPLETED    0x0406
#define CELL_PERF_SIGNAL_PPU_TH0_FPU_LOAD_INSTRUCTION_COMPLETED                0x0407
#define CELL_PERF_SIGNAL_PPU_TH0_VMX_LOAD_INSTRUCTION_COMPLETED                0x0408
#define CELL_PERF_SIGNAL_PPU_TH0_VMX_STORE_INSTRUCTION_COMPLETED               0x0409
#define CELL_PERF_SIGNAL_PPU_TH0_VIQ_STALL_DUE_TO_FP_DENORMALIZE               0x040c
#define CELL_PERF_SIGNAL_PPU_TH0_VMX_FP_PRODUCED_NAN_OR_INFINITY               0x040f

/* PPU VMX/FPU — thread 1 */
#define CELL_PERF_SIGNAL_PPU_TH1_TWO_VMX_OR_SCALAR_INSTRUCTION_COMPLETED       0x0410
#define CELL_PERF_SIGNAL_PPU_TH1_SLOT0_INSTRUCTION_COMPLETED                   0x0411
#define CELL_PERF_SIGNAL_PPU_TH1_SLOT1_INSTRUCTION_COMPLETED                   0x0412
#define CELL_PERF_SIGNAL_PPU_TH1_VMX_SIMPLE_PIPELINE_INSTRUCTION_COMPLETED     0x0413
#define CELL_PERF_SIGNAL_PPU_TH1_VMX_FLOAT_PIPELINE_INSTRUCTION_COMPLETED      0x0414
#define CELL_PERF_SIGNAL_PPU_TH1_RECIPROCAL_ESTIMATE_INSTRUCTION_COMPLETED     0x0415
#define CELL_PERF_SIGNAL_PPU_TH1_VMX_PERMUTE_PIPELINE_INSTRUCTION_COMPLETED    0x0416
#define CELL_PERF_SIGNAL_PPU_TH1_FPU_LOAD_INSTRUCTION_COMPLETED                0x0417
#define CELL_PERF_SIGNAL_PPU_TH1_VMX_LOAD_INSTRUCTION_COMPLETED                0x0418
#define CELL_PERF_SIGNAL_PPU_TH1_VMX_STORE_INSTRUCTION_COMPLETED               0x0419
#define CELL_PERF_SIGNAL_PPU_TH1_VIQ_STALL_DUE_TO_FP_DENORMALIZE               0x041c
#define CELL_PERF_SIGNAL_PPU_TH1_VMX_FP_PRODUCED_NAN_OR_INFINITY               0x041f

/* PPU MMU */
#define CELL_PERF_SIGNAL_PPU_MMU_SLB_MISS                                      0x051b
#define CELL_PERF_SIGNAL_PPU_MMU_SLBIE_REQUEST                                 0x051c
#define CELL_PERF_SIGNAL_PPU_MMU_SLBIA_REQUEST                                 0x051d

/* MFC L2 — group A */
#define CELL_PERF_SIGNAL_L2_HIT                                                0x0b00
#define CELL_PERF_SIGNAL_L2_MISS                                               0x0b01
#define CELL_PERF_SIGNAL_L2_ALL_RC_BUSY                                        0x0b02
#define CELL_PERF_SIGNAL_L2_ALL_CO_BUSY                                        0x0b03
#define CELL_PERF_SIGNAL_L2_LOAD_MISS                                          0x0b04
#define CELL_PERF_SIGNAL_L2_STORE_MISS                                         0x0b05
#define CELL_PERF_SIGNAL_L2_INST_DEMAND_MISS                                   0x0b06

/* MFC L2 — group A, thread 0 RC breakdowns */
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH0                               0x0b07
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH0_RC0                           0x0b08
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH0_RC1                           0x0b09
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH0_RC2                           0x0b0a
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH0_RC3                           0x0b0b
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH0_RC4                           0x0b0c
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH0_RC5                           0x0b0d
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH0                               0x0b0e
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH0_RC0                           0x0b0f
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH0_RC1                           0x0b10
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH0_RC2                           0x0b11
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH0_RC3                           0x0b12
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH0_RC4                           0x0b13
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH0_RC5                           0x0b14
#define CELL_PERF_SIGNAL_L2_PREFETCH                                           0x0b15
#define CELL_PERF_SIGNAL_L2_PREFETCH_MISS                                      0x0b16
#define CELL_PERF_SIGNAL_L2_PREFETCH_HIT                                       0x0b17
#define CELL_PERF_SIGNAL_L2_INTERVENTION_RECEIVED                              0x0b18
#define CELL_PERF_SIGNAL_L2_ALL_SNOOP_MACHINES_BUSY                            0x0b19

/* MFC L2 — group B */
#define CELL_PERF_SIGNAL_L2_RC_DIR_WRITE_LOCKOUT                               0x0c00
#define CELL_PERF_SIGNAL_L2_L1_DATA_INVALIDATE                                 0x0c01
#define CELL_PERF_SIGNAL_L2_PTEG_MISS                                          0x0c02
#define CELL_PERF_SIGNAL_L2_CASTOUT                                            0x0c03
#define CELL_PERF_SIGNAL_L2_DCLAIM_GOOD                                        0x0c04
#define CELL_PERF_SIGNAL_L2_DATA_DEMAND_COLLISION                              0x0c05
#define CELL_PERF_SIGNAL_L2_DATA_PREFETCH_HIT_RC_WORKING                       0x0c06
#define CELL_PERF_SIGNAL_L2_INST_PREFETCH_HIT_RC_WORKING                       0x0c07
#define CELL_PERF_SIGNAL_L2_RC_NOT_DISPATCHED_DUETO_ANOTHER_RC                 0x0c08
#define CELL_PERF_SIGNAL_L2_LD_ST_RC_RETRIED_DUETO_SNOOP_ADDR                  0x0c09
#define CELL_PERF_SIGNAL_L2_LD_ST_RC_RETRIED_DUETO_NO_ACK                      0x0c0a
#define CELL_PERF_SIGNAL_L2_DCLAIM_CONVERTED_INTO_RWITM                        0x0c0b
#define CELL_PERF_SIGNAL_L2_STORE_TO_M_MU_E                                    0x0c0c
#define CELL_PERF_SIGNAL_L2_STORE_Q_FULL                                       0x0c0d
#define CELL_PERF_SIGNAL_L2_STORE_DISPATCHED_RC                                0x0c0e
#define CELL_PERF_SIGNAL_L2_GATHERABLE_STORE                                   0x0c0f
#define CELL_PERF_SIGNAL_L2_SNOOPED                                            0x0c10
#define CELL_PERF_SIGNAL_L2_L1_INVALIDATED_DUETO_SNOOP                         0x0c11
#define CELL_PERF_SIGNAL_L2_SNOOP_PUSH                                         0x0c12
#define CELL_PERF_SIGNAL_L2_INTERVENTION_FROM_SL_E_SAME_NODE                   0x0c13
#define CELL_PERF_SIGNAL_L2_INTERVANTION_FROM_M_MU_SAME_NODE                   0x0c14
#define CELL_PERF_SIGNAL_L2_INTERVENTION_FROM_T_SAME_NODE                      0x0c15
#define CELL_PERF_SIGNAL_L2_RESPOND_RETRY                                      0x0c19
#define CELL_PERF_SIGNAL_L2_RESPOND_RETRY_DUETO_SNOOP_BUSY                     0x0c1a
#define CELL_PERF_SIGNAL_L2_SNOOPED_RESP_MMU_TO_EST_TRANSITION                 0x0c1b
#define CELL_PERF_SIGNAL_L2_SNOOPED_RESP_E_TO_S_TRANSITION                     0x0c1c
#define CELL_PERF_SIGNAL_L2_SNOOPED_RESP_ESLST_TO_I_TRANSITION                 0x0c1d
#define CELL_PERF_SIGNAL_L2_SNOOPED_RESP_MMU_TO_I_TRANSITION                   0x0c1e
#define CELL_PERF_SIGNAL_L2_DISPATCH_SNOOP_MACHINE                             0x0c1f

/* MFC L2 — group C */
#define CELL_PERF_SIGNAL_L2_RC0_BUSY_WITH_MISS                                 0x0d00
#define CELL_PERF_SIGNAL_L2_RC1_BUSY_WITH_MISS                                 0x0d01
#define CELL_PERF_SIGNAL_L2_RC2_BUSY_WITH_MISS                                 0x0d02
#define CELL_PERF_SIGNAL_L2_RC3_BUSY_WITH_MISS                                 0x0d03
#define CELL_PERF_SIGNAL_L2_RC4_BUSY_WITH_MISS                                 0x0d04
#define CELL_PERF_SIGNAL_L2_RC5_BUSY_WITH_MISS                                 0x0d05
#define CELL_PERF_SIGNAL_L2_RC0_BUSY_WITH_DCLAIM                               0x0d06
#define CELL_PERF_SIGNAL_L2_RC1_BUSY_WITH_DCLAIM                               0x0d07
#define CELL_PERF_SIGNAL_L2_RC2_BUSY_WITH_DCLAIM                               0x0d08
#define CELL_PERF_SIGNAL_L2_RC3_BUSY_WITH_DCLAIM                               0x0d09
#define CELL_PERF_SIGNAL_L2_RC4_BUSY_WITH_DCLAIM                               0x0d0a
#define CELL_PERF_SIGNAL_L2_RC5_BUSY_WITH_DCLAIM                               0x0d0b

/* MFC L2 — group C, thread 1 RC breakdowns */
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH1                               0x0d0c
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH1_RC0                           0x0d0d
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH1_RC1                           0x0d0e
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH1_RC2                           0x0d0f
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH1_RC3                           0x0d10
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH1_RC4                           0x0d11
#define CELL_PERF_SIGNAL_L2_LD_AND_RESV_MISS_TH1_RC5                           0x0d12
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH1                               0x0d13
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH1_RC0                           0x0d14
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH1_RC1                           0x0d15
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH1_RC2                           0x0d16
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH1_RC3                           0x0d17
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH1_RC4                           0x0d18
#define CELL_PERF_SIGNAL_L2_ST_AND_COND_MISS_TH1_RC5                           0x0d19

/* MFC L2 — group D */
#define CELL_PERF_SIGNAL_L2_CACHE_READ_BLOCKED                                 0x0e00
#define CELL_PERF_SIGNAL_L2_CACHE_WRITE_BLOCKED                                0x0e01
#define CELL_PERF_SIGNAL_L2_INST_DEMAND_FETCH                                  0x0e02
#define CELL_PERF_SIGNAL_L2_MISS_MODIFIED                                      0x0e03
#define CELL_PERF_SIGNAL_L2_MISS_SHARED                                        0x0e04
#define CELL_PERF_SIGNAL_L2_MISS_SHARED_OR_TAGGED                              0x0e05
#define CELL_PERF_SIGNAL_L2_MISS_TAGGED                                        0x0e06
#define CELL_PERF_SIGNAL_L2_RWITM_OR_DCLAIM_RETRY                              0x0e07
#define CELL_PERF_SIGNAL_L2_MISS_RETRY                                         0x0e08
#define CELL_PERF_SIGNAL_L2_LD_ST_RC_CASTOUT_ADDR_COLLISION                    0x0e09
#define CELL_PERF_SIGNAL_L2_STORE_Q_NOT_EMPTY                                  0x0e0c
#define CELL_PERF_SIGNAL_L2_STORE_Q_WAITING_FOR_RC                             0x0e0d

/* MFC NCU */
#define CELL_PERF_SIGNAL_NCU_SYNC_REQ                                          0x0f01
#define CELL_PERF_SIGNAL_NCU_NON_CACHEABLE_STORE_REQ                           0x0f04
#define CELL_PERF_SIGNAL_NCU_EIEIO_REQ                                         0x0f06
#define CELL_PERF_SIGNAL_NCU_TLBIE_REQ                                         0x0f07
#define CELL_PERF_SIGNAL_NCU_SYNC_WAIT                                         0x0f08
#define CELL_PERF_SIGNAL_NCU_LWSYNC_WAIT                                       0x0f09
#define CELL_PERF_SIGNAL_NCU_EIEIO_WAIT                                        0x0f0a
#define CELL_PERF_SIGNAL_NCU_TLBIE_WAIT                                        0x0f0b
#define CELL_PERF_SIGNAL_NCU_COMBINED_NON_CACHEABLE_STORE                      0x0f0c
#define CELL_PERF_SIGNAL_NCU_ALL_STORE_GATHER_BUFFERS_FULL                     0x0f0f
#define CELL_PERF_SIGNAL_NCU_NON_CACHEABLE_LOAD_REQ                            0x0f10
#define CELL_PERF_SIGNAL_NCU_STORE_QUEUE_NOT_EMPTY                             0x0f11
#define CELL_PERF_SIGNAL_NCU_STORE_QUEUE_FULL                                  0x0f12
#define CELL_PERF_SIGNAL_NCU_AT_LEAST_ONE_GATHER_BUFFER_NOT_EMPTY              0x0f13

/* SPU monitor */
#define CELL_PERF_SIGNAL_SPU_DUAL_INSTRUCTION_COMPLETED                        0x1000
#define CELL_PERF_SIGNAL_SPU_SINGLE_INSTRUCTION_COMPLETED                      0x1001
#define CELL_PERF_SIGNAL_SPU_PIPE0_INSTRUCTION_COMPLETED                       0x1002
#define CELL_PERF_SIGNAL_SPU_PIPE1_INSTRUCTION_COMPLETED                       0x1003
#define CELL_PERF_SIGNAL_SPU_IFETCH_STALL                                      0x1004
#define CELL_PERF_SIGNAL_SPU_LS_BUSY                                           0x1005
#define CELL_PERF_SIGNAL_SPU_DMA_CONFLICT_WITH_LD_ST                           0x1006
#define CELL_PERF_SIGNAL_SPU_STORE_TO_LS                                       0x1007
#define CELL_PERF_SIGNAL_SPU_LOAD_FROM_LS                                      0x1008
#define CELL_PERF_SIGNAL_SPU_FPU_EXCEPTION                                     0x1009
#define CELL_PERF_SIGNAL_SPU_BRANCH_INSTRUCTION_COMPLETED                      0x100a
#define CELL_PERF_SIGNAL_SPU_BRANCH_TAKEN                                      0x100b
#define CELL_PERF_SIGNAL_SPU_BRANCH_NOT_TAKEN                                  0x100c
#define CELL_PERF_SIGNAL_SPU_BRANCH_MISS_PREDICTION                            0x100d
#define CELL_PERF_SIGNAL_SPU_BRANCH_HINT_MISS_PREDICTION                       0x100e
#define CELL_PERF_SIGNAL_SPU_INSTRUCTION_SEQUENCE_ERROR                        0x100f
#define CELL_PERF_SIGNAL_SPU_CHANNEL_READ_STALL                                0x1010
#define CELL_PERF_SIGNAL_SPU_CHANNEL_WRITE_STALL                               0x1011
#define CELL_PERF_SIGNAL_SPU_CHANNEL_0_STALL                                   0x1012
#define CELL_PERF_SIGNAL_SPU_CHANNEL_3_STALL                                   0x1013
#define CELL_PERF_SIGNAL_SPU_CHANNEL_4_STALL                                   0x1014
#define CELL_PERF_SIGNAL_SPU_CHANNEL_21_STALL                                  0x1015
#define CELL_PERF_SIGNAL_SPU_CHANNEL_24_STALL                                  0x1016
#define CELL_PERF_SIGNAL_SPU_CHANNEL_25_STALL                                  0x1017
#define CELL_PERF_SIGNAL_SPU_CHANNEL_28_STALL                                  0x1018
#define CELL_PERF_SIGNAL_SPU_CHANNEL_30_STALL                                  0x1019
#define CELL_PERF_SIGNAL_SPU_TAG_COMPLETION_WAIT                               0x101a
#define CELL_PERF_SIGNAL_SPU_LS_DATA_ECC_ERROR                                 0x101b
#define CELL_PERF_SIGNAL_SPU_STOP_INSTRUCTION                                  0x101c
#define CELL_PERF_SIGNAL_SPU_HALT_INSTRUCTION                                  0x101d
#define CELL_PERF_SIGNAL_SPU_CHANNEL_29_STALL                                  0x101e
#define CELL_PERF_SIGNAL_SPU_EVENT_WAIT_STALL                                  0x101f

/* SMF atomic */
#define CELL_PERF_SIGNAL_ATOMIC_RC_MACHINE_BUSY                                0x1300
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_RECEIVED_FROM_DMAC                        0x1301
#define CELL_PERF_SIGNAL_ATOMIC_DCLAIM_SENT_TO_SBI                             0x1302
#define CELL_PERF_SIGNAL_ATOMIC_RWITM_PERFORMED_SENT_SBI                       0x1303
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_MISS_CAUSED_MU_STATE                      0x1304
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_MISS_CAUSED_E_STATE                       0x1305
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_MISS_CAUSED_SL_STATE                      0x1306
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_HITS_CACHE                                0x1307
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_MISS_WITH_INTERVENTION                    0x1308
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_MISS_WITHOUT_INTERVENTION                 0x1309
#define CELL_PERF_SIGNAL_ATOMIC_LOAD_GETS_RETRY_RESPONSE                       0x130a
#define CELL_PERF_SIGNAL_ATOMIC_PUTLLC_OR_PUTLLUC_HITS_E_M_MU_STATE            0x130b
#define CELL_PERF_SIGNAL_ATOMIC_PUTLLC_OR_PUTLLUC_HITS_S_SL_T_STATE            0x130c
#define CELL_PERF_SIGNAL_ATOMIC_PUTLLC_OR_PUTLLUC_MISSES_WITH_INTERVENTION     0x130d
#define CELL_PERF_SIGNAL_ATOMIC_PUTLLC_OR_PUTLLUC_MISSES_WITHOUT_INTERVENTION  0x130e
#define CELL_PERF_SIGNAL_ATOMIC_PUTLLC_OR_PUTLLUC_GETS_RETRY_RESPONSE          0x130f
#define CELL_PERF_SIGNAL_ATOMIC_CASTOUT_MACHINE_BUSY                           0x1310
#define CELL_PERF_SIGNAL_ATOMIC_SNOOP_MACHINE_BUSY                             0x1311
#define CELL_PERF_SIGNAL_ATOMIC_SNOOP_CAUSED_TRANSITION_FROM_MMU_TO_ES_STATE   0x1312
#define CELL_PERF_SIGNAL_ATOMIC_SNOOP_CAUSED_TRANSITION_FROM_MMU_TO_I_STATE    0x1313
#define CELL_PERF_SIGNAL_ATOMIC_SNOOP_CAUSED_TRANSITION_FROM_T_TO_S_STATE      0x1314
#define CELL_PERF_SIGNAL_ATOMIC_SNOOP_CAUSED_TRANSITION_FROM_ESSL_TO_I_STATE   0x1315
#define CELL_PERF_SIGNAL_ATOMIC_SNOOP_CAUSED_TRANSITION_FROM_T_TO_I_STATE      0x1316
#define CELL_PERF_SIGNAL_ATOMIC_SNOOP_CAUSED_TRANSITION_FROM_MU_TO_T_STATE     0x1317
#define CELL_PERF_SIGNAL_ATOMIC_SENT_RESOURCE_OR_COHERENCY_RETRY               0x1318
#define CELL_PERF_SIGNAL_ATOMIC_SENT_SHARED_DATA_INTERVENTION_TO_LOCAL_CBE     0x1319
#define CELL_PERF_SIGNAL_ATOMIC_SENT_MODIFIED_DATA_INTERVENTION_TO_LOCAL_CBE   0x131b
#define CELL_PERF_SIGNAL_ATOMIC_SENT_TAGGED_DATA_INTERVENTION_TO_LOCAL_CBE     0x131d
#define CELL_PERF_SIGNAL_ATOMIC_PUTLLC_OR_PUTLLUC_IS_SUCCESSFUL                0x131f

/* SMF DMA — group 1 */
#define CELL_PERF_SIGNAL_DMAC_ANY_DMA_GET_ISSUED                               0x1400
#define CELL_PERF_SIGNAL_DMAC_ANY_DMA_PUT_ISSUED                               0x1401
#define CELL_PERF_SIGNAL_DMAC_DMA_PUT_ISSUED                                   0x1402
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTS_ISSUED                                  0x1403
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTR_ISSUED                                  0x1404
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTF_ISSUED                                  0x1405
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTB_ISSUED                                  0x1406
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTFS_ISSUED                                 0x1407
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTBS_ISSUED                                 0x1408
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTRF_ISSUED                                 0x1409
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTRB_ISSUED                                 0x140a
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTL_ISSUED                                  0x140b
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTRL_ISSUED                                 0x140c
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTLF_ISSUED                                 0x140d
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTLB_ISSUED                                 0x140e
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTRLF_ISSUED                                0x140f
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTRLB_ISSUED                                0x1410
#define CELL_PERF_SIGNAL_DMAC_DMA_GET_ISSUED                                   0x1411
#define CELL_PERF_SIGNAL_DMAC_DMA_GETS_ISSUED                                  0x1412
#define CELL_PERF_SIGNAL_DMAC_DMA_GETF_ISSUED                                  0x1413
#define CELL_PERF_SIGNAL_DMAC_DMA_GETB_ISSUED                                  0x1414
#define CELL_PERF_SIGNAL_DMAC_DMA_GETFS_ISSUED                                 0x1415
#define CELL_PERF_SIGNAL_DMAC_DMA_GETBS_ISSUED                                 0x1416
#define CELL_PERF_SIGNAL_DMAC_DMA_GETL_ISSUED                                  0x1417
#define CELL_PERF_SIGNAL_DMAC_DMA_GETLF_ISSUED                                 0x1418
#define CELL_PERF_SIGNAL_DMAC_DMA_GETLB_ISSUED                                 0x1419
#define CELL_PERF_SIGNAL_DMAC_DMA_PUT_IN_TCLASS_0_ISSUED                       0x141a
#define CELL_PERF_SIGNAL_DMAC_DMA_GET_IN_TCLASS_0_ISSUED                       0x141b
#define CELL_PERF_SIGNAL_DMAC_DMA_PUT_IN_TCLASS_1_ISSUED                       0x141c
#define CELL_PERF_SIGNAL_DMAC_DMA_GET_IN_TCLASS_1_ISSUED                       0x141d
#define CELL_PERF_SIGNAL_DMAC_DMA_PUT_IN_TCLASS_2_ISSUED                       0x141e
#define CELL_PERF_SIGNAL_DMAC_DMA_GET_IN_TCLASS_2_ISSUED                       0x141f

/* SMF DMA — group 2 (queue / barrier / MMIO events) */
#define CELL_PERF_SIGNAL_DMAC_PPU_QUEUE_NOT_EMPTY                              0x1500
#define CELL_PERF_SIGNAL_DMAC_PPU_QUEUE_FULL                                   0x1501
#define CELL_PERF_SIGNAL_DMAC_SPU_QUEUE_NOT_EMPTY                              0x1502
#define CELL_PERF_SIGNAL_DMAC_SPU_QUEUE_FULL                                   0x1503
#define CELL_PERF_SIGNAL_DMAC_RECEIVED_MMIO_READ                               0x1504
#define CELL_PERF_SIGNAL_DMAC_RECEIVED_MMIO_WRITE                              0x1505
#define CELL_PERF_SIGNAL_DMAC_DMA_SNDSIG_ISSUED                                0x1506
#define CELL_PERF_SIGNAL_DMAC_DMA_SNDSIGF_ISSUED                               0x1507
#define CELL_PERF_SIGNAL_DMAC_DMA_SNDSIGB_ISSUED                               0x1508
#define CELL_PERF_SIGNAL_DMAC_DMA_BARRIER_ISSUED                               0x1509
#define CELL_PERF_SIGNAL_DMAC_DMA_MFCSYNC_ISSUED                               0x150a
#define CELL_PERF_SIGNAL_DMAC_DMA_MFCEIEIO_ISSUED                              0x150b
#define CELL_PERF_SIGNAL_DMAC_DMA_SDCRZ_ISSUED                                 0x150c
#define CELL_PERF_SIGNAL_DMAC_DMA_SDCRS_ISSUED                                 0x150d
#define CELL_PERF_SIGNAL_DMAC_DMA_SDCRF_ISSUED                                 0x150e
#define CELL_PERF_SIGNAL_DMAC_DMA_GETLLAR_ISSUED                               0x150f
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTLLC_ISSUED                                0x1510
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTLLUC_ISSUED                               0x1511
#define CELL_PERF_SIGNAL_DMAC_DMA_PUTQLLUC_ISSUED                              0x1512

/* SMF SBI */
#define CELL_PERF_SIGNAL_SBI_SENT_RETRY_TO_GCMD_DUE_TO_WR_Q_FULL               0x1600
#define CELL_PERF_SIGNAL_SBI_SENT_RETRY_TO_LCMD_DUE_TO_WR_Q_FULL               0x1601
#define CELL_PERF_SIGNAL_SBI_SENT_RETRY_TO_GCMD_DUE_TO_RD_Q_FULL               0x1602
#define CELL_PERF_SIGNAL_SBI_SENT_RETRY_TO_LCMD_DUE_TO_RD_Q_FULL               0x1603
#define CELL_PERF_SIGNAL_SBI_SENT_LOAD_COMMAND                                 0x1604
#define CELL_PERF_SIGNAL_SBI_SENT_STORE_COMMAND                                0x1605
#define CELL_PERF_SIGNAL_SBI_RECEIVED_DATA                                     0x1606
#define CELL_PERF_SIGNAL_SBI_SENT_DATA                                         0x1607
#define CELL_PERF_SIGNAL_SBI_REQUEST_QUEUE_NOT_EMPTY                           0x1608
#define CELL_PERF_SIGNAL_SBI_REQUEST_QUEUE_FULL                                0x1609
#define CELL_PERF_SIGNAL_SBI_SENT_COMMAND_REQUEST                              0x160a
#define CELL_PERF_SIGNAL_SBI_NO_COMMAND_CREDIT_FOR_REQUEST                     0x160b
#define CELL_PERF_SIGNAL_SBI_RECEIVED_DATA_BUS_GRANT                           0x160c
#define CELL_PERF_SIGNAL_SBI_CYCLES_BETWEEN_DREQ_AND_DGRANT                    0x160d
#define CELL_PERF_SIGNAL_SBI_SENT_COMMAND_FOR_ODD_MEMORY_BANK                  0x160e
#define CELL_PERF_SIGNAL_SBI_SENT_COMMAND_FOR_EVEN_MEMORY_BANK                 0x160f
#define CELL_PERF_SIGNAL_SBI_SENT_COMMAND_FOR_IOIF                             0x1610
#define CELL_PERF_SIGNAL_SBI_SENT_COMMAND_FOR_NO_TOKEN                         0x1611
#define CELL_PERF_SIGNAL_SBI_RECEIVED_RETRY_RESPONSE                           0x1612
#define CELL_PERF_SIGNAL_SBI_SENT_DATA_BUS_REQUEST                             0x1613
#define CELL_PERF_SIGNAL_SBI_SNET_MFCEIEIO_EVENT                               0x1614
#define CELL_PERF_SIGNAL_SBI_SENT_MFCEIEIO_CYCLE                               0x1615
#define CELL_PERF_SIGNAL_SBI_SENT_MFCSYNC_EVENT                                0x1616
#define CELL_PERF_SIGNAL_SBI_SENT_MFCSYNC_CYCLE                                0x1617

/* EIB DARB — group 1 */
#define CELL_PERF_SIGNAL_EIB_GRANT_ON_DATA_RING_0                              0x1c00
#define CELL_PERF_SIGNAL_EIB_GRANT_ON_DATA_RING_1                              0x1c01
#define CELL_PERF_SIGNAL_EIB_GRANT_ON_DATA_RING_2                              0x1c02
#define CELL_PERF_SIGNAL_EIB_GRANT_ON_DATA_RING_3                              0x1c03
#define CELL_PERF_SIGNAL_EIB_DATA_RING_0_WAS_IN_USE                            0x1c04
#define CELL_PERF_SIGNAL_EIB_DATA_RING_1_WAS_IN_USE                            0x1c05
#define CELL_PERF_SIGNAL_EIB_DATA_RING_2_WAS_IN_USE                            0x1c06
#define CELL_PERF_SIGNAL_EIB_DATA_RING_3_WAS_IN_USE                            0x1c07
#define CELL_PERF_SIGNAL_EIB_ALL_DATA_RINGS_WERE_IDLE                          0x1c08
#define CELL_PERF_SIGNAL_EIB_ONE_DATA_RING_WAS_BUSY                            0x1c09
#define CELL_PERF_SIGNAL_EIB_TWO_OR_THREE_DATA_RINGS_WERE_BUSY                 0x1c0a
#define CELL_PERF_SIGNAL_EIB_ALL_DATA_RINGS_WERE_BUSY                          0x1c0b

/* EIB DARB — group 2 */
#define CELL_PERF_SIGNAL_EIB_IOIF0_DATA_REQUEST_PENDING                        0x1d00
#define CELL_PERF_SIGNAL_EIB_SPE6_DATA_REQUEST_PENDING                         0x1d01
#define CELL_PERF_SIGNAL_EIB_SPE4_DATA_REQUEST_PENDING                         0x1d02
#define CELL_PERF_SIGNAL_EIB_SPE2_DATA_REQUEST_PENDING                         0x1d03
#define CELL_PERF_SIGNAL_EIB_SPE0_DATA_REQUEST_PENDING                         0x1d04
#define CELL_PERF_SIGNAL_EIB_MIC_DATA_REQUEST_PENDING                          0x1d05
#define CELL_PERF_SIGNAL_EIB_PPE_DATA_REQUEST_PENDING                          0x1d06
#define CELL_PERF_SIGNAL_EIB_SPE1_DATA_REQUEST_PENDING                         0x1d07
#define CELL_PERF_SIGNAL_EIB_SPE3_DATA_REQUEST_PENDING                         0x1d08
#define CELL_PERF_SIGNAL_EIB_SPE5_DATA_REQUEST_PENDING                         0x1d09
#define CELL_PERF_SIGNAL_EIB_SPE7_DATA_REQUEST_PENDING                         0x1d0a
#define CELL_PERF_SIGNAL_EIB_IOIF1_DATA_REQUEST_PENDING                        0x1d0b
#define CELL_PERF_SIGNAL_EIB_IOIF0_WAS_DATA_DESTINATION                        0x1d0c
#define CELL_PERF_SIGNAL_EIB_SPE6_WAS_DATA_DESTINATION                         0x1d0d
#define CELL_PERF_SIGNAL_EIB_SPE4_WAS_DATA_DESTINATION                         0x1d0e
#define CELL_PERF_SIGNAL_EIB_SPE2_WAS_DATA_DESTINATION                         0x1d0f
#define CELL_PERF_SIGNAL_EIB_SPE0_WAS_DATA_DESTINATION                         0x1d10
#define CELL_PERF_SIGNAL_EIB_MIC_WAS_DATA_DESTINATION                          0x1d11
#define CELL_PERF_SIGNAL_EIB_PPE_WAS_DATA_DESTINATION                          0x1d12
#define CELL_PERF_SIGNAL_EIB_SPE1_WAS_DATA_DESTINATION                         0x1d13
#define CELL_PERF_SIGNAL_EIB_SPE3_WAS_DATA_DESTINATION                         0x1d14
#define CELL_PERF_SIGNAL_EIB_SPE5_WAS_DATA_DESTINATION                         0x1d15
#define CELL_PERF_SIGNAL_EIB_SPE7_WAS_DATA_DESTINATION                         0x1d16
#define CELL_PERF_SIGNAL_EIB_IOIF1_WAS_DATA_DESTINATION                        0x1d17

/* MIC — YAC 0 */
#define CELL_PERF_SIGNAL_MIC_YAC0_CMD_CACHE_ADDR_COLLISION_IN_Q                0x3300
#define CELL_PERF_SIGNAL_MIC_YAC0_READ_CMD_DISPATCHED_BEFORE_CRESP             0x3301
#define CELL_PERF_SIGNAL_MIC_LEVEL1_BACKPRESSURE_FROM_RD_CMD_Q                 0x3302
#define CELL_PERF_SIGNAL_MIC_LEVEL1_BACKPRESSURE_FROM_WR_CMD_Q                 0x3303
#define CELL_PERF_SIGNAL_MIC_LEVEL2_BACKPRESSURE_FROM_RD_CMD_Q                 0x3304
#define CELL_PERF_SIGNAL_MIC_LEVEL2_BACKPRESSURE_FROM_WR_CMD_Q                 0x3305
#define CELL_PERF_SIGNAL_MIC_LEVEL3_BACKPRESSURE_FROM_RD_CMD_Q                 0x3306
#define CELL_PERF_SIGNAL_MIC_LEVEL3_BACKPRESSURE_FROM_WR_CMD_Q                 0x3307
#define CELL_PERF_SIGNAL_MIC_YAC0_WR_CMD_Q_ABOVE_HIGH_WATER_MARK               0x3308
#define CELL_PERF_SIGNAL_MIC_YAC0_READ_CMD_Q_EMPTY                             0x3309
#define CELL_PERF_SIGNAL_MIC_YAC0_WRITE_CMD_Q_EMPTY                            0x330a
#define CELL_PERF_SIGNAL_MIC_YAC0_BOTH_RD_AND_WR_Q_EMPTY                       0x330b
#define CELL_PERF_SIGNAL_MIC_YAC0_READ_CMD_Q_FULL                              0x330c
#define CELL_PERF_SIGNAL_MIC_YAC0_MIC_RESPONDS_READ_RETRY                      0x330d
#define CELL_PERF_SIGNAL_MIC_YAC0_WRITE_CMD_Q_FULL                             0x330e
#define CELL_PERF_SIGNAL_MIC_YAC0_MIC_RESPONDS_WRITE_RETRY                     0x330f
#define CELL_PERF_SIGNAL_MIC_YAC0_HIGH_PRIORITY_READ_CMD_DISPATCHED            0x3320
#define CELL_PERF_SIGNAL_MIC_YAC0_WR_CMD_DISPATCHED_WITH_PENDING_RD            0x3321
#define CELL_PERF_SIGNAL_MIC_YAC0_READ_CMD_DISPATCHED                          0x3322
#define CELL_PERF_SIGNAL_MIC_YAC0_WRITE_CMD_DISPATCHED                         0x3323
#define CELL_PERF_SIGNAL_MIC_YAC0_RD_MODIFY_WR_CMD_DISPATCHED                  0x3324
#define CELL_PERF_SIGNAL_MIC_YAC0_REFRESH_DISPATCHED                           0x3325
#define CELL_PERF_SIGNAL_MIC_YAC0_SCRUB_DISPATCHED                             0x3326
#define CELL_PERF_SIGNAL_MIC_YAC0_BYTE_MASK_WRITE_CMD_DISPATCHED               0x3327
#define CELL_PERF_SIGNAL_MIC_YAC0_HIGH_PRIORITY_READ_CMD_WAITING               0x3328
#define CELL_PERF_SIGNAL_MIC_YAC0_WR_CMD_DISPATCHED_AFTER_RD_CMD               0x3329
#define CELL_PERF_SIGNAL_MIC_YAC0_RD_CMD_DISPATCHED_AFTER_WR_CMD               0x332a
#define CELL_PERF_SIGNAL_MIC_YAC0_RD_MODIFY_WR_CMD_Q_FULL                      0x332c

/* MIC — YAC 1 */
#define CELL_PERF_SIGNAL_MIC_YAC1_CMD_CACHE_ADDR_COLLISION_IN_Q                0x3200
#define CELL_PERF_SIGNAL_MIC_YAC1_READ_CMD_DISPATCHED_BEFORE_CRESP             0x3201
#define CELL_PERF_SIGNAL_MIC_YAC1_READ_DATA_RCVD_FROM_MEM                      0x3204
#define CELL_PERF_SIGNAL_MIC_YAC1_READ_CMD_RETRY_BEFORE_DATA_RCVD              0x3205
#define CELL_PERF_SIGNAL_MIC_YAC1_READ_CMD_RETRY_AFTER_DATA_RCVD               0x3206
#define CELL_PERF_SIGNAL_MIC_HIGH_PRIORITY_READ_CMD_RECEIVED                   0x3207
#define CELL_PERF_SIGNAL_MIC_YAC1_WR_CMD_Q_ABOVE_HIGH_WATER_MARK               0x3208
#define CELL_PERF_SIGNAL_MIC_YAC1_READ_CMD_Q_EMPTY                             0x3209
#define CELL_PERF_SIGNAL_MIC_YAC1_WRITE_CMD_Q_EMPTY                            0x320a
#define CELL_PERF_SIGNAL_MIC_YAC1_BOTH_RD_AND_WR_Q_EMPTY                       0x320b
#define CELL_PERF_SIGNAL_MIC_YAC1_READ_CMD_Q_FULL                              0x320c
#define CELL_PERF_SIGNAL_MIC_YAC1_MIC_RESPONDS_READ_RETRY                      0x320d
#define CELL_PERF_SIGNAL_MIC_YAC1_WRITE_CMD_Q_FULL                             0x320e
#define CELL_PERF_SIGNAL_MIC_YAC1_MIC_RESPONDS_WRITE_RETRY                     0x320f
#define CELL_PERF_SIGNAL_MIC_YAC1_HIGH_PRIORITY_READ_CMD_DISPATCHED            0x3220
#define CELL_PERF_SIGNAL_MIC_YAC1_WR_CMD_DISPATCHED_WITH_PENDING_RD            0x3221
#define CELL_PERF_SIGNAL_MIC_YAC1_READ_CMD_DISPATCHED                          0x3222
#define CELL_PERF_SIGNAL_MIC_YAC1_WRITE_CMD_DISPATCHED                         0x3223
#define CELL_PERF_SIGNAL_MIC_YAC1_RD_MODIFY_WR_CMD_DISPATCHED                  0x3224
#define CELL_PERF_SIGNAL_MIC_YAC1_REFRESH_DISPATCHED                           0x3225
#define CELL_PERF_SIGNAL_MIC_YAC1_SCRUB_DISPATCHED                             0x3226
#define CELL_PERF_SIGNAL_MIC_YAC1_BYTE_MASK_WRITE_CMD_DISPATCHED               0x3227
#define CELL_PERF_SIGNAL_MIC_YAC1_HIGH_PRIORITY_READ_CMD_WAITING               0x3228
#define CELL_PERF_SIGNAL_MIC_YAC1_WR_CMD_DISPATCHED_AFTER_RD_CMD               0x3229
#define CELL_PERF_SIGNAL_MIC_YAC1_RD_CMD_DISPATCHED_AFTER_WR_CMD               0x322a
#define CELL_PERF_SIGNAL_MIC_YAC1_RD_MODIFY_WR_CMD_Q_FULL                      0x322c

/* BIC — IOIF0 outbound */
#define CELL_PERF_SIGNAL_BIC_TYPE_A_DATA_TRANSFER_TO_IOIF0                     0x3512
#define CELL_PERF_SIGNAL_BIC_TYPE_B_DATA_TRANSFER_TO_IOIF0                     0x3513

/* BIC — IOIF1 outbound */
#define CELL_PERF_SIGNAL_BIC_TYPE_A_DATA_TRANSFER_TO_IOIF1                     0x3612
#define CELL_PERF_SIGNAL_BIC_TYPE_B_DATA_TRANSFER_TO_IOIF1                     0x3613

/* BIC — IOIF0 inbound */
#define CELL_PERF_SIGNAL_BIC_TYPE_A_DATA_TRANSFER_FROM_IOIF0                   0x371d
#define CELL_PERF_SIGNAL_BIC_TYPE_B_DATA_TRANSFER_FROM_IOIF0                   0x371e

/* BIC — IOIF1 inbound */
#define CELL_PERF_SIGNAL_BIC_TYPE_A_DATA_TRANSFER_FROM_IOIF1                   0x381d
#define CELL_PERF_SIGNAL_BIC_TYPE_B_DATA_TRANSFER_FROM_IOIF1                   0x381e

/* IOC — group 1 */
#define CELL_PERF_SIGNAL_IOC_RECEIVED_READ_REQUEST_TO_IOIF1                    0x3908
#define CELL_PERF_SIGNAL_IOC_RECEIVED_WRITE_REQUEST_TO_IOIF1                   0x3909
#define CELL_PERF_SIGNAL_IOC_RECEIVED_READ_REQUEST_TO_IOIF0                    0x390a
#define CELL_PERF_SIGNAL_IOC_RECEIVED_WRITE_REQUEST_TO_IOIF0                   0x390b
#define CELL_PERF_SIGNAL_IOC_SENT_COMMAND_TO_IOIF0                             0x390c
#define CELL_PERF_SIGNAL_IOC_SENT_COMMAND_TO_IOIF1                             0x390d

/* IOC — group 2 */
#define CELL_PERF_SIGNAL_IOC_RECEIVED_READ_REQUEST_FROM_IOIF0                  0x3a0a
#define CELL_PERF_SIGNAL_IOC_RECEIVED_WRITE_REQUEST_FROM_IOIF0                 0x3a0b
#define CELL_PERF_SIGNAL_IOC_RECEIVED_READ_REQUEST_FROM_IOIF1                  0x3a0c
#define CELL_PERF_SIGNAL_IOC_RECEIVED_WRITE_REQUEST_FROM_IOIF1                 0x3a0d
#define CELL_PERF_SIGNAL_IOC_RECEIVED_INTERRUPT_FROM_IOIF0                     0x3a0e
#define CELL_PERF_SIGNAL_IOC_RECEIVED_INTERRUPT_FROM_IOIF1                     0x3a0f

#define CELL_PERF_NUM_SIGNALS                    450

/* ------------------------------------------------------------------ *
 * Trace header flags (trace data 16-bit header field)
 * ------------------------------------------------------------------ */

#define CELL_PERF_TRACE_HEADER_TA_OVERFLOW       0x8000
#define CELL_PERF_TRACE_HEADER_ANY_BKMK          0x4000
#define CELL_PERF_TRACE_HEADER_SPU_BKMK0         0x2000
#define CELL_PERF_TRACE_HEADER_SPU_BKMK1         0x1000
#define CELL_PERF_TRACE_HEADER_SPU_ADDR0         0x0800
#define CELL_PERF_TRACE_HEADER_SPU_ADDR1         0x0400
#define CELL_PERF_TRACE_HEADER_PPU0_BKMK         0x0200
#define CELL_PERF_TRACE_HEADER_PPU1_BKMK         0x0100

/* ------------------------------------------------------------------ *
 * Error codes
 * ------------------------------------------------------------------ */

#define CELL_PERF_ERROR_NOT_OPENED                  CELL_ERROR_CAST(0x80520001)
#define CELL_PERF_ERROR_INVALID_SIGNAL              CELL_ERROR_CAST(0x80520002)
#define CELL_PERF_ERROR_TRACE_BUS_BUSY              CELL_ERROR_CAST(0x80520003)
#define CELL_PERF_ERROR_SIGNAL_CONTROL_FULL         CELL_ERROR_CAST(0x80520004)
#define CELL_PERF_ERROR_INVALID_PARAMETER           CELL_ERROR_CAST(0x80520005)
#define CELL_PERF_ERROR_LV1_NO_PRIVILEGE            CELL_ERROR_CAST(0x80520006)
#define CELL_PERF_ERROR_LV1_DENIED_BY_POLICY        CELL_ERROR_CAST(0x80520007)
#define CELL_PERF_ERROR_LV1_NO_ENTRY                CELL_ERROR_CAST(0x80520008)
#define CELL_PERF_ERROR_LV1_DUPLICATE_ENTRY         CELL_ERROR_CAST(0x80520009)
#define CELL_PERF_ERROR_LV1_BUSY                    CELL_ERROR_CAST(0x8052000a)
#define CELL_PERF_ERROR_LV1_RESOURCE_SHORTAGE       CELL_ERROR_CAST(0x8052000b)
#define CELL_PERF_ERROR_LV1_WRONG_STATE             CELL_ERROR_CAST(0x8052000c)
#define CELL_PERF_ERROR_LV1_ALIGNMENT_ERROR         CELL_ERROR_CAST(0x8052000d)
#define CELL_PERF_ERROR_LV1_ILLEGAL_PARAMETER_VALUE CELL_ERROR_CAST(0x8052000e)
#define CELL_PERF_ERROR_LV2_BUSY                    CELL_ERROR_CAST(0x8052000f)
#define CELL_PERF_ERROR_LV2_NO_RESOURCE             CELL_ERROR_CAST(0x80520010)
#define CELL_PERF_ERROR_LV2_NOT_OPENED              CELL_ERROR_CAST(0x80520011)
#define CELL_PERF_ERROR_LV2_NOT_IMPLEMENTED         CELL_ERROR_CAST(0x80520012)
#define CELL_PERF_ERROR_LV2_INVALID_PARAMETER       CELL_ERROR_CAST(0x80520013)
#define CELL_PERF_ERROR_LV2_UNSUPPORTED_HARDWARE    CELL_ERROR_CAST(0x80520014)
#define CELL_PERF_ERROR_LV2_NOT_ALLOWED             CELL_ERROR_CAST(0x80520015)
#define CELL_PERF_ERROR_UNDEFINED                   CELL_ERROR_CAST(0x80520016)
#define CELL_PERF_ERROR_NO_RESOURCE                 CELL_ERROR_CAST(0x80520017)
#define CELL_PERF_ERROR_ADDED_ALREADY               CELL_ERROR_CAST(0x80520018)
#define CELL_PERF_ERROR_NO_ENTRY                    CELL_ERROR_CAST(0x80520019)

/* ------------------------------------------------------------------ *
 * SPU target specification
 * ------------------------------------------------------------------ */

#define CELL_PERF_SPU_TARGET_TYPE_THREAD_ID         0
#define CELL_PERF_SPU_TARGET_TYPE_THREAD_NAME       1
#define CELL_PERF_SPU_TARGET_TYPE_LOGICAL_ID        2
#define CELL_PERF_SPU_TARGET_TYPE_PHYSICAL_ID       3

/* ------------------------------------------------------------------ *
 * LV2 OS trace constants
 * ------------------------------------------------------------------ */

#define CELL_PERF_LV2_TRACE_TYPE_CONTROL            0xf0
#define CELL_PERF_LV2_TRACE_TYPE_PPU_SCHEDULER      0x00
#define CELL_PERF_LV2_TRACE_TYPE_PPU_THREAD         0x03
#define CELL_PERF_LV2_TRACE_TYPE_SPU_SCHEDULER      0x10
#define CELL_PERF_LV2_TRACE_TYPE_SPU_THREAD         0x13
#define CELL_PERF_LV2_TRACE_TYPE_SYSTEM_CALL        0x33

#define CELL_PERF_LV2_TRACE_MODE_STOP_AT_END        0x00
#define CELL_PERF_LV2_TRACE_MODE_OVERWRITE          0x01
#define CELL_PERF_LV2_TRACE_MODE_BACKTRACE_MASK     0x0000FF00
#define CELL_PERF_LV2_TRACE_MODE_BACKTRACE_SHIFT    8

#define CELL_PERF_LV2_TRACE_CONTROL_START           0x00000001
#define CELL_PERF_LV2_TRACE_CONTROL_STOP            0x00000002

/* LV2 OS trace — scheduler incidents */
#define CELL_PERF_LV2OS_TRACE_SCHED_DISPATCHED      0x01
#define CELL_PERF_LV2OS_TRACE_SCHED_PREEMPTED       0x02
#define CELL_PERF_LV2OS_TRACE_SCHED_YIELD           0x03
#define CELL_PERF_LV2OS_TRACE_SCHED_SLEEP           0x04
#define CELL_PERF_LV2OS_TRACE_SCHED_EXIT            0x05
#define CELL_PERF_LV2OS_TRACE_SCHED_SET_PRIO        0x10
#define CELL_PERF_LV2OS_TRACE_SCHED_INHERIT_PRIO    0x20
#define CELL_PERF_LV2OS_TRACE_SCHED_DISINHERIT_PRIO 0x30

/* LV2 OS trace — thread incidents */
#define CELL_PERF_LV2OS_TRACE_THREAD_CREATED        0x00
#define CELL_PERF_LV2OS_TRACE_THREAD_DELETED        0x01
#define CELL_PERF_LV2OS_TRACE_THREAD_EXISTS         0x02

/* LV2 OS trace — syscall type */
#define CELL_PERF_LV2OS_TRACE_SYSCALL_TYPE_ENTRY    0x00
#define CELL_PERF_LV2OS_TRACE_SYSCALL_TYPE_EXIT     0x10

/* LV2 OS trace — syscall context */
#define CELL_PERF_LV2OS_TRACE_SYSCALL_CTX_PPU       0x00
#define CELL_PERF_LV2OS_TRACE_SYSCALL_CTX_HANDLER   0x01
#define CELL_PERF_LV2OS_TRACE_SYSCALL_CTX_SPU       0x10

#define CELL_PERF_LV2_TRACE_MAX_THREAD_NAME         (255 + 1)
#define CELL_PERF_LV2_TRACE_MAX_BACKTRACE           \
	(CELL_PERF_LV2_TRACE_MODE_BACKTRACE_MASK >> \
	 CELL_PERF_LV2_TRACE_MODE_BACKTRACE_SHIFT)

#define CELL_PERF_LV2_TRACE_HEADER_SIZE             128

/* ------------------------------------------------------------------ *
 * Type definitions
 * ------------------------------------------------------------------ */

/* 128-bit trace data element */
typedef union CellPerfTraceData {
	uint64_t u64[2];
	uint32_t u32[4];
	uint16_t u16[8];
	uint8_t  u8[16];
} CellPerfTraceData;

/* Signal name (group << 8 | index) */
typedef uint32_t CellPerfSignalName;

/* Signal setup word (enable / polarity / trigger-edge) */
typedef uint32_t CellPerfSignalSetup;

/* ------------------------------------------------------------------ *
 * Struct types
 * ------------------------------------------------------------------ */

typedef struct CellPerfSpuTarget {
	uint32_t         type;
	sys_spu_thread_t threadId;
	char             threadName[CELL_PERF_MAX_THREAD_NAME_LENGTH];
	uint32_t         logicalId;
	uint32_t         physicalId;
} CellPerfSpuTarget;

typedef struct CellPerfSignal {
	CellPerfSignalName  name;
	CellPerfSignalSetup setup;
	uint32_t            targetSpuId;
	uint32_t            mask;
} CellPerfSignal;

typedef struct CellPerfCBEpmSetup {
	uint32_t          conf;
	uint32_t          interval;
	CellPerfSignal    signal[CELL_PERF_NUM_16BIT_COUNTERS];
	CellPerfSpuTarget spuTraceTarget[CELL_PERF_MAX_SPU_TARGET];
} CellPerfCBEpmSetup;

typedef struct CellPerfCBEpmStatus {
	uint32_t          traceDataSize;
	CellPerfTraceData *traceDataCacheAddress ATTRIBUTE_PRXPTR;
	uint32_t          counter[CELL_PERF_NUM_32BIT_COUNTERS];
	uint32_t          interval;
	uint64_t          startSystemTime;
	uint64_t          stopSystemTime;
} CellPerfCBEpmStatus;

typedef union CellPerfCBEpmCounter {
	uint32_t counter32[CELL_PERF_NUM_32BIT_COUNTERS];
	uint16_t counter16[CELL_PERF_NUM_16BIT_COUNTERS];
} CellPerfCBEpmCounter;

/* ------------------------------------------------------------------ *
 * LV2 OS trace types
 * ------------------------------------------------------------------ */

typedef struct CellPerfLv2TraceHeader {
	uint64_t writePointer;
	uint64_t readPointer;
	uint64_t reserved[CELL_PERF_LV2_TRACE_HEADER_SIZE / sizeof(uint64_t) - 2];
} CellPerfLv2TraceHeader;

typedef struct CellPerfLv2TracePacketHeader {
	uint8_t  tag;
	uint8_t  length;
	uint16_t hardwareId;
	uint32_t timebase;
} CellPerfLv2TracePacketHeader;

typedef struct CellPerfLv2TraceControlPacketPayload {
	uint32_t control;
	uint32_t reserved;
} CellPerfLv2TraceControlPacketPayload;

typedef struct CellPerfLv2TraceSchedPacketPayload {
	uint32_t threadId;
	uint8_t  incident;
	uint8_t  reserved;
	uint16_t priority;
} CellPerfLv2TraceSchedPacketPayload;

typedef struct CellPerfLv2TraceThreadPacketPayload {
	uint32_t processId;
	uint32_t threadId;
	uint8_t  nameLength;
	uint8_t  incident;
	uint16_t reserved;
	char     threadName[CELL_PERF_LV2_TRACE_MAX_THREAD_NAME];
} CellPerfLv2TraceThreadPacketPayload;

typedef struct CellPerfLv2TraceSyscallInPacketPayload {
	uint8_t  type;
	uint8_t  context;
	uint16_t reserved;
	uint32_t threadId;
	uint64_t syscallNumber;
	uint32_t srr0;
	uint32_t lr[CELL_PERF_LV2_TRACE_MAX_BACKTRACE + 1];
} CellPerfLv2TraceSyscallInPacketPayload;

typedef struct CellPerfLv2TraceSyscallOutPacketPayload {
	uint8_t  type;
	uint8_t  context;
	uint16_t reserved;
	uint32_t threadId;
	uint64_t gpr3;
} CellPerfLv2TraceSyscallOutPacketPayload;

typedef union CellPerfLv2TracePacketPayload {
	CellPerfLv2TraceControlPacketPayload    control;
	CellPerfLv2TraceSchedPacketPayload      schedule;
	CellPerfLv2TraceThreadPacketPayload     thread;
	CellPerfLv2TraceSyscallInPacketPayload  syscallIn;
	CellPerfLv2TraceSyscallOutPacketPayload syscallOut;
} CellPerfLv2TracePacketPayload;

typedef struct CellPerfLv2TracePacket {
	CellPerfLv2TracePacketHeader  header;
	CellPerfLv2TracePacketPayload payload;
} CellPerfLv2TracePacket;

/* ------------------------------------------------------------------ *
 * Backward-compatible API surface (pre-CBEpm naming)
 * ------------------------------------------------------------------ */

#ifndef CELL_PERF_API_CHANGE

typedef struct CellPerfSetup {
	uint32_t          conf;
	uint32_t          traceDataCacheSize;
	uint32_t          interval;
	CellPerfSignal    signals[CELL_PERF_NUM_16BIT_COUNTERS];
	CellPerfSpuTarget spuTraceTarget[CELL_PERF_MAX_SPU_TARGET];
} CellPerfSetup;

typedef CellPerfCBEpmStatus  CellPerfStatus;
typedef CellPerfCBEpmCounter CellPerfCounter;

#endif /* CELL_PERF_API_CHANGE */

/* ------------------------------------------------------------------ *
 * Function prototypes
 * ------------------------------------------------------------------ */

#ifdef __cplusplus
extern "C" {
#endif

/* Common */
void     cellPerfStart(void);
void     cellPerfStop(void);
void     cellPerfDeleteAll(void);
uint32_t cellPerfGetVersion(void);

/* BE Performance Monitor */
int  cellPerfAddCBEpm(CellPerfCBEpmSetup *setup);
void cellPerfDeleteCBEpm(void);
int  cellPerfGetCBEpmStatus(CellPerfCBEpmStatus *status);
int  cellPerfCopyCBEpmTraceData(uint32_t offset);
int  cellPerfCopyCBEpmTraceDataDma(uint32_t offset);
int  cellPerfGetCBEpmWritePointer(uint32_t *writePointer);
void cellPerfInsertCBEpmBookmark(uint64_t bookmark);
int  cellPerfReadAndResetCBEpmCounter(CellPerfCBEpmCounter *counter);
int  cellPerfChangeCBEpmCacheSize(uint64_t size);

/* LV2 OS Trace */
int  cellPerfAddLv2OSTrace(uint32_t type, size_t bufSize, uint32_t mode,
                           uint32_t **buf);
void cellPerfDeleteAllLv2OSTrace(void);
void cellPerfDeleteLv2OSTrace(uint32_t type);

#ifndef CELL_PERF_API_CHANGE

/* Backward-compatible API */
int  cellPerfOpen(CellPerfSetup *setup);
int  cellPerfClose(void);
int  cellPerfEnable(void);
int  cellPerfDisable(void);
int  cellPerfGetStatus(CellPerfStatus *status);
int  cellPerfCopyTraceData(uint32_t offset, uint32_t size);
void cellPerfInsertBookmark(uint64_t bookmark);
int  cellPerfReadAndResetCounter(CellPerfCounter *counter);

#endif /* CELL_PERF_API_CHANGE */

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_PERF_PERFORMANCE_H__ */
