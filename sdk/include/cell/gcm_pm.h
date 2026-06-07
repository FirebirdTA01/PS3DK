/*! \file cell/gcm_pm.h
 \brief Cell-style libgcm performance monitor declaration surface.

  This header intentionally exposes only the API shape needed by source that
  includes <cell/gcm_pm.h>.  Per-domain hardware event IDs are not shipped here
  until they can be sourced through an independent public reference.
*/

#ifndef __PS3TC_CELL_GCM_PM_H__
#define __PS3TC_CELL_GCM_PM_H__

#include <stdint.h>
#include <cell/error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CellGcmPerfMonDomain {
    CELL_GCM_PM_DOMAIN_HCLK = 0,
    CELL_GCM_PM_DOMAIN_GCLK,
    CELL_GCM_PM_DOMAIN_RCLK,
    CELL_GCM_PM_DOMAIN_SCLK,
    CELL_GCM_PM_DOMAIN_MCLK,
    CELL_GCM_PM_DOMAIN_ICLK,
    CELL_GCM_PM_DOMAIN_NUM
} CellGcmPerfMonDomain;

typedef uint32_t CellGcmPerfMonHclk;
typedef uint32_t CellGcmPerfMonGclk;
typedef uint32_t CellGcmPerfMonRclk;
typedef uint32_t CellGcmPerfMonSclk;
typedef uint32_t CellGcmPerfMonMclk;
typedef uint32_t CellGcmPerfMonIclk;

enum {
    CELL_GCM_PM_MAX_EVENT = 4,
    CELL_GCM_PM_MAX_COUNTER = 4
};

#define CELL_GCM_ERROR_PM_STATE_INVALID CELL_ERROR_CAST(0x802100f1)

int32_t cellGcmInitPerfMon(void);
int32_t cellGcmExitPerfMon(void);
int32_t cellGcmSetPerfMonEvent(uint32_t domain,
                               const uint32_t event[CELL_GCM_PM_MAX_EVENT]);
int32_t cellGcmGetPerfMonCounter(uint32_t domain,
                                 uint32_t counter[CELL_GCM_PM_MAX_COUNTER],
                                 uint32_t *cycle);

#ifdef __cplusplus
}
#endif

#endif /* __PS3TC_CELL_GCM_PM_H__ */
