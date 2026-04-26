/* SPU-side mirror of the request-word layout.  Kept as a separate
 * header purely so the spu/ subtree stays compilable without leaking
 * PPU include paths into the spu-elf-gcc command line. */

#ifndef HELLO_EVENT_FLAG_SPU_PROTOCOL_SPU_H
#define HELLO_EVENT_FLAG_SPU_PROTOCOL_SPU_H

#define SPU_REQ_OPCODE_SHIFT     8u
#define SPU_REQ_BIT_MASK         0xffu

#define SPU_REQ_OP_SET_BIT       0x01u
#define SPU_REQ_OP_SET_BIT_IMP   0x02u
#define SPU_REQ_OP_QUIT          0xffu

#define SPU_REQ_OPCODE(req)      (((req) >> SPU_REQ_OPCODE_SHIFT) & 0xffu)
#define SPU_REQ_BITIDX(req)      ((req) & SPU_REQ_BIT_MASK)

#endif /* HELLO_EVENT_FLAG_SPU_PROTOCOL_SPU_H */
