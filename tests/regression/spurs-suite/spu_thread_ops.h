/* spurs-suite SPU thread operations: values shared by both sides. */
#ifndef SPURS_SUITE_SPU_THREAD_OPS_H
#define SPURS_SUITE_SPU_THREAD_OPS_H

#define STO_MAGIC    0x57a0c0deu   /* first word of the box once the SPU runs */
#define STO_PORT     1             /* SPU port of the user event */
#define STO_IN       0x00123400u   /* the PPU writes this to ls_in */
#define STO_M1       0x56u         /* first mailbox word */
#define STO_M2       0x2au         /* second mailbox word = the exit code */

#endif
