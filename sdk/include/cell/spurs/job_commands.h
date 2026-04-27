/* cell/spurs/job_commands.h - SPURS job-list opcode encoding.
 *
 * A job chain is a uint64_t[] terminated by a CELL_SPURS_JOB_COMMAND_END
 * entry; non-zero low bits 0..2 mark a control opcode, all-zero low
 * bits mean "the high 61 bits are a job-descriptor EA".  The
 * COMMAND_xxx macros pack EAs / labels into the right form.
 */
#ifndef __PS3DK_CELL_SPURS_JOB_COMMANDS_H__
#define __PS3DK_CELL_SPURS_JOB_COMMANDS_H__

#include <stdint.h>

/* Three-bit opcode (low 3 bits): 0 = job-descriptor EA, 1..6 control
 * commands, 7 = extended (high nibble of the 7-bit ext code lives in
 * bits 3..6 of the command word). */
#define CELL_SPURS_JOB_OPCODE_NOP            0
#define CELL_SPURS_JOB_OPCODE_RESET_PC       1
#define CELL_SPURS_JOB_OPCODE_SYNC           (2 | (0 << 3))
#define CELL_SPURS_JOB_OPCODE_LWSYNC         (2 | (2 << 3))
#define CELL_SPURS_JOB_OPCODE_SYNC_LABEL     (2 | (1 << 3))
#define CELL_SPURS_JOB_OPCODE_LWSYNC_LABEL   (2 | (3 << 3))
#define CELL_SPURS_JOB_OPCODE_NEXT           3
#define CELL_SPURS_JOB_OPCODE_CALL           4
#define CELL_SPURS_JOB_OPCODE_FLUSH          5
#define CELL_SPURS_JOB_OPCODE_JOBLIST        6

/* Extended opcodes - low 7 bits all set => bits 3..6 select. */
#define CELL_SPURS_JOB_OPCODE_ABORT          (7 | (0  << 3))
#define CELL_SPURS_JOB_OPCODE_GUARD          (7 | (1  << 3))
#define CELL_SPURS_JOB_OPCODE_SET_LABEL      (7 | (2  << 3))
#define CELL_SPURS_JOB_OPCODE_RET            (7 | (14 << 3))
#define CELL_SPURS_JOB_OPCODE_END            (7 | (15 << 3))

/* JTS = "jump to source" - lwsync + a one-bit "stop here" flag in
 * bit 35.  Used at the head of a chain to keep the SPU stalled until
 * the PPU explicitly runs the chain. */
#define CELL_SPURS_JOB_OPCODE_JTS            (0x800000000ull | CELL_SPURS_JOB_OPCODE_LWSYNC)

#define CELL_SPURS_JOB_COMMAND_NOP                  CELL_SPURS_JOB_OPCODE_NOP
#define CELL_SPURS_JOB_COMMAND_JOB(ea)              ((uint64_t)(uintptr_t)(ea) & ~7ull)
#define CELL_SPURS_JOB_COMMAND_JOBLIST(ea)          (CELL_SPURS_JOB_OPCODE_JOBLIST   | ((uint64_t)(uintptr_t)(ea) & ~7ull))
#define CELL_SPURS_JOB_COMMAND_RESET_PC(ea)         (CELL_SPURS_JOB_OPCODE_RESET_PC  | ((uint64_t)(uintptr_t)(ea) & ~7ull))
#define CELL_SPURS_JOB_COMMAND_GUARD(ea)            (CELL_SPURS_JOB_OPCODE_GUARD     | ((uint64_t)(uintptr_t)(ea) & ~127ull))
#define CELL_SPURS_JOB_COMMAND_SYNC                 CELL_SPURS_JOB_OPCODE_SYNC
#define CELL_SPURS_JOB_COMMAND_LWSYNC               CELL_SPURS_JOB_OPCODE_LWSYNC
#define CELL_SPURS_JOB_COMMAND_SYNC_LABEL(label)    (CELL_SPURS_JOB_OPCODE_SYNC_LABEL   | ((uint64_t)(label) << 7))
#define CELL_SPURS_JOB_COMMAND_LWSYNC_LABEL(label)  (CELL_SPURS_JOB_OPCODE_LWSYNC_LABEL | ((uint64_t)(label) << 7))
#define CELL_SPURS_JOB_COMMAND_NEXT(ea)             (CELL_SPURS_JOB_OPCODE_NEXT  | ((uint64_t)(uintptr_t)(ea) & ~7ull))
#define CELL_SPURS_JOB_COMMAND_CALL(ea)             (CELL_SPURS_JOB_OPCODE_CALL  | ((uint64_t)(uintptr_t)(ea) & ~7ull))
#define CELL_SPURS_JOB_COMMAND_FLUSH                CELL_SPURS_JOB_OPCODE_FLUSH
#define CELL_SPURS_JOB_COMMAND_RET                  CELL_SPURS_JOB_OPCODE_RET
#define CELL_SPURS_JOB_COMMAND_ABORT                CELL_SPURS_JOB_OPCODE_ABORT
#define CELL_SPURS_JOB_COMMAND_END                  CELL_SPURS_JOB_OPCODE_END
#define CELL_SPURS_JOB_COMMAND_SET_LABEL(label)     (CELL_SPURS_JOB_OPCODE_SET_LABEL | ((uint64_t)(label) << 7))
#define CELL_SPURS_JOB_COMMAND_JTS                  CELL_SPURS_JOB_OPCODE_JTS

#define cellSpursJobGetOpcode(c)         ((uint32_t)((c) & 7))
#define cellSpursJobGetOpcodeExt(c)      ((uint32_t)((c) & 127))
#define cellSpursJobIsJob(c)             ((((c) & 7) == 0) && (c) != 0)
#define cellSpursJobIsCommandExt(c)      (cellSpursJobGetOpcode(c) == 7)
#define cellSpursJobGetAddress8(c)       ((c) & 0xFFFFFFFFFFFFFFF8ull)
#define cellSpursJobGetAddress128(c)     ((c) & 0xFFFFFFFFFFFFFF80ull)

#endif /* __PS3DK_CELL_SPURS_JOB_COMMANDS_H__ */
