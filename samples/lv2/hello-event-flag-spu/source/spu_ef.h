/* hello-event-flag-spu — shared protocol between the PPU master and the
 * SPU worker.
 *
 * The master sends a 32-bit "request" word and a 32-bit event-flag-id
 * word to the SPU via two signal-notification channels.  The SPU
 * decodes the request (which carries both an opcode and a target bit
 * index), performs the matching sys_event_flag_set_bit{,_impatient}
 * call, then loops back to read the next request.  An opcode of
 * SPU_REQ_QUIT tells the SPU to drop into sys_spu_thread_exit().
 *
 * Bit layout of the request word:
 *
 *   31         8 7      0
 *  +-----------+--------+
 *  |   opcode  | bitidx |
 *  +-----------+--------+
 *
 * Bit index occupies the low byte so a single shift unpacks it.  The
 * opcode field deliberately leaves room for future extensions (e.g.
 * "set + clear" or "broadcast") without renumbering.
 */

#ifndef HELLO_EVENT_FLAG_SPU_PROTOCOL_H
#define HELLO_EVENT_FLAG_SPU_PROTOCOL_H

#define SPU_REQ_OPCODE_SHIFT     8u
#define SPU_REQ_BIT_MASK         0xffu

#define SPU_REQ_OP_SET_BIT       0x01u
#define SPU_REQ_OP_SET_BIT_IMP   0x02u
#define SPU_REQ_OP_QUIT          0xffu

#define SPU_REQ_BUILD(op, bit) \
    ((((unsigned)(op)) << SPU_REQ_OPCODE_SHIFT) | ((unsigned)(bit) & SPU_REQ_BIT_MASK))

#define SPU_REQ_OPCODE(req)      (((req) >> SPU_REQ_OPCODE_SHIFT) & 0xffu)
#define SPU_REQ_BITIDX(req)      ((req) & SPU_REQ_BIT_MASK)

/* Bit assignments inside the 64-bit event flag mask. */
#define EF_BIT_PPU_WORKER_BASE   0u    /* PPU workers occupy bits 0..N-1 */
#define EF_BIT_SPU_WORKER        6u    /* SPU worker reports completion here */

#define NUM_PPU_WORKERS          5

#endif /* HELLO_EVENT_FLAG_SPU_PROTOCOL_H */
