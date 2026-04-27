/*! \file cell/padfilter.h
 \brief libpadfilter — second-order IIR low-pass filter for pad axis data.

  Clean-room reproduction of the cellPadFilter API surface based on the
  SCE PlayStation(R)3 Programmer Tool Runtime Library 475 header that
  shipped with the reference SDK.  The filter is a generic Butterworth
  second-order section (biquad) operating on uint32_t samples — pad
  axis values come in as 32-bit words from cellPadGetData(), and a
  smoothed value comes out per Filter() call.

  This is a header-only declaration; the implementation lives in
  libpadfilter.a (Phase 6.5 stub-archive flow).
*/

#ifndef PS3TC_CELL_PADFILTER_H
#define PS3TC_CELL_PADFILTER_H

#include <stdint.h>
#include <cell/error.h>

#define CELL_PADFILTER_OK                          CELL_OK
#define CELL_PADFILTER_ERROR_INVALID_PARAMETER     0x80121401

/* Cutoff-frequency selectors for cellPadFilterIIRInit().
 * fc is given as a fraction of the sampling Nyquist frequency. */
enum {
    CELL_PADFILTER_IIR_CUTOFF_2ND_LPF_BT_050 = 0,  /* fc = 0.50 * f-nyquist */
    CELL_PADFILTER_IIR_CUTOFF_2ND_LPF_BT_020 = 1,  /* fc = 0.20 * f-nyquist */
    CELL_PADFILTER_IIR_CUTOFF_2ND_LPF_BT_010 = 2   /* fc = 0.10 * f-nyquist */
};

typedef struct CellPadFilterIIRSos {
    int32_t u[3];   /* unit-delay state */
    int32_t a1;
    int32_t a2;
    int32_t b0;
    int32_t b1;
    int32_t b2;
} CellPadFilterIIRSos;

#ifdef __cplusplus
extern "C" {
#endif

int32_t  cellPadFilterIIRInit  (CellPadFilterIIRSos *sos, int32_t cutoff);
uint32_t cellPadFilterIIRFilter(CellPadFilterIIRSos *sos, uint32_t in);

#ifdef __cplusplus
}
#endif

#endif  /* PS3TC_CELL_PADFILTER_H */
