/* spurs-suite SPU vector literals: values shared by both sides. */
#ifndef SPURS_SUITE_VECTOR_LITERALS_H
#define SPURS_SUITE_VECTOR_LITERALS_H

#define VL_MAGIC   0x7ec11700u   /* first word of the box once the SPU is done */
#define VL_X       0x1234        /* the run-time scalar the SPU splats */
#define VL_CHECKS  9             /* bits of the failure mask */

#endif
