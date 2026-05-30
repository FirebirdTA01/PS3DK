#ifndef __PS3DK_CELL_NP_SIGNALING_H__
#define __PS3DK_CELL_NP_SIGNALING_H__

#include <cell/np/common.h>
#include <netinet/in.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCE_NP_SIGNALING_CTX_MAX 8

#define SCE_NP_SIGNALING_EVENT_DEAD 0
#define SCE_NP_SIGNALING_EVENT_ESTABLISHED 1
#define SCE_NP_SIGNALING_EVENT_NETINFO_ERROR 2
#define SCE_NP_SIGNALING_EVENT_NETINFO_RESULT 3
#define SCE_NP_SIGNALING_EVENT_EXT_PEER_ACTIVATED 10
#define SCE_NP_SIGNALING_EVENT_EXT_PEER_DEACTIVATED 11
#define SCE_NP_SIGNALING_EVENT_EXT_MUTUAL_ACTIVATED 12
#define SCE_NP_SIGNALING_EVENT_EXT_ACTIVATED 10

#define SCE_NP_SIGNALING_CONN_STATUS_INACTIVE 0
#define SCE_NP_SIGNALING_CONN_STATUS_PENDING 1
#define SCE_NP_SIGNALING_CONN_STATUS_ACTIVE 2

#define SCE_NP_SIGNALING_CONN_INFO_RTT 1
#define SCE_NP_SIGNALING_CONN_INFO_BANDWIDTH 2
#define SCE_NP_SIGNALING_CONN_INFO_PEER_NPID 3
#define SCE_NP_SIGNALING_CONN_INFO_PEER_ADDRESS 4
#define SCE_NP_SIGNALING_CONN_INFO_MAPPED_ADDRESS 5
#define SCE_NP_SIGNALING_CONN_INFO_PACKET_LOSS 6

#define SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE 1
#define SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE_ENABLE 1
#define SCE_NP_SIGNALING_CTX_OPT_BANDWIDTH_PROBE_DISABLE 0

#define SCE_NP_SIGNALING_NETINFO_NAT_STATUS_UNKNOWN 0
#define SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE1 1
#define SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE2 2
#define SCE_NP_SIGNALING_NETINFO_NAT_STATUS_TYPE3 3
#define SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_UNKNOWN 0
#define SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_INVALID 1
#define SCE_NP_SIGNALING_NETINFO_UPNP_STATUS_VALID 2
#define SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_CLOSED 0
#define SCE_NP_SIGNALING_NETINFO_NPPORT_STATUS_OPEN 1

typedef union SceNpSignalingConnectionInfo {
    uint32_t rtt;
    uint32_t bandwidth;
    SceNpId npId;
    struct {
        struct in_addr addr;
        in_port_t port;
    } address;
    uint32_t packet_loss;
} SceNpSignalingConnectionInfo;

typedef void (*SceNpSignalingHandler)(uint32_t ctx_id, uint32_t subject_id, int event, int error_code, void *arg);

typedef struct SceNpSignalingNetInfo {
    uint32_t size;
    struct in_addr local_addr;
    struct in_addr mapped_addr;
    int32_t nat_status;
    int32_t upnp_status;
    int32_t npport_status;
    uint16_t npport;
    uint8_t padding[2];
} SceNpSignalingNetInfo;

static inline int sceNpSignalingInit(void) { return 0; }
static inline int sceNpSignalingTerm(void) { return 0; }

int sceNpSignalingCreateCtx(const SceNpId *npId, SceNpSignalingHandler handler, void *arg, uint32_t *ctx_id);
int sceNpSignalingDestroyCtx(uint32_t ctx_id);
int sceNpSignalingActivateConnection(uint32_t ctx_id, const SceNpId *npId, uint32_t *conn_id);
int sceNpSignalingDeactivateConnection(uint32_t ctx_id, uint32_t conn_id);
int sceNpSignalingTerminateConnection(uint32_t ctx_id, uint32_t conn_id);
int sceNpSignalingGetConnectionStatus(uint32_t ctx_id, uint32_t conn_id, int *conn_status, struct in_addr *peer_addr, in_port_t *peer_port);
int sceNpSignalingGetConnectionInfo(uint32_t ctx_id, uint32_t conn_id, int code, SceNpSignalingConnectionInfo *info);
int sceNpSignalingAddExtendedHandler(uint32_t ctx_id, SceNpSignalingHandler handler, void *arg);
int sceNpSignalingGetConnectionFromNpId(uint32_t ctx_id, const SceNpId *npId, uint32_t *conn_id);
int sceNpSignalingGetConnectionFromPeerAddress(uint32_t ctx_id, struct in_addr peer_addr, in_port_t peer_port, uint32_t *conn_id);
int sceNpSignalingSetCtxOpt(uint32_t ctx_id, int optname, int optval);
int sceNpSignalingGetCtxOpt(uint32_t ctx_id, int optname, int *optval);
int sceNpSignalingGetLocalNetInfo(uint32_t ctx_id, SceNpSignalingNetInfo *netinfo);
int sceNpSignalingGetPeerNetInfo(uint32_t ctx_id, const SceNpId *npId, uint32_t *req_id);
int sceNpSignalingCancelPeerNetInfo(uint32_t ctx_id, uint32_t req_id);
int sceNpSignalingGetPeerNetInfoResult(uint32_t ctx_id, uint32_t req_id, SceNpSignalingNetInfo *netinfo);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_NP_SIGNALING_H__ */
