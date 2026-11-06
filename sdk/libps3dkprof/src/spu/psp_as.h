/* Raw MFC_RdAtomicStat -> the core's abstract status bits, for the operation just issued (design I3 r2 section 1).
 * Raw meanings (spu_mfcio.h MFC_*_STATUS and the architecture's MFC_RdAtomicStat definition; target behaviour is
 * measured only at the hardware gate):
 *   after getllar: 4 (GETLLAR complete);  after putllc: 0 = stored, 1 = reservation lost;  after putlluc: 2.
 * Exact equality: any other value, including reserved, combined or another operation's bits, returns 0, which fails
 * the core's check at that call site and latches FAULT. */
#ifndef PSP_AS_H
#define PSP_AS_H

#include <stdint.h>

#include "psp_spu.h"

enum { PSP_OP_GETLLAR = 1, PSP_OP_PUTLLC = 2, PSP_OP_PUTLLUC = 3 };

#define PSP_RAW_PUTLLC_FAILED 1u
#define PSP_RAW_PUTLLUC_DONE 2u
#define PSP_RAW_GETLLAR_DONE 4u

static inline uint32_t psp_as_translate(int op, uint32_t raw)
{
    switch (op) {
    case PSP_OP_GETLLAR:
        return raw == PSP_RAW_GETLLAR_DONE ? PSP_AS_GETLLAR : 0u;
    case PSP_OP_PUTLLC:
        return raw == 0u ? PSP_AS_PUTLLC_OK : raw == PSP_RAW_PUTLLC_FAILED ? PSP_AS_PUTLLC_LOST : 0u;
    case PSP_OP_PUTLLUC:
        return raw == PSP_RAW_PUTLLUC_DONE ? PSP_AS_PUTLLUC : 0u;
    default:
        return 0u;
    }
}

#endif
