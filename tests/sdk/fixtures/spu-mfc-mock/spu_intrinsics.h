/* Host stand-in for spu_intrinsics.h (see spu_mfcio.h beside it). */
#ifndef MFC_MOCK_SPU_INTRINSICS_H
#define MFC_MOCK_SPU_INTRINSICS_H

#define spu_hcmpeq(a, b) ((void)(a), (void)(b))

#endif /* MFC_MOCK_SPU_INTRINSICS_H */
