/*
 * cell/rudp.h - reliable UDP declarations.
 *
 * Backed by librudp_stub.a emitted from
 * tools/nidgen/nids/extracted/librudp_stub.yaml. Link with
 * -lrudp_stub and load CELL_SYSMODULE_RUDP before making calls.
 */
#ifndef __PS3DK_CELL_RUDP_H__
#define __PS3DK_CELL_RUDP_H__

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t CellRudpUsec;

typedef struct CellRudpAllocator {
    void *(*app_malloc)(size_t size) ATTRIBUTE_PRXPTR;
    void (*app_free)(void *ptr) ATTRIBUTE_PRXPTR;
} CellRudpAllocator;

typedef int (*CellRudpEventHandler)(int event_id,
                                    int soc,
                                    const uint8_t *data,
                                    size_t len,
                                    const struct sockaddr *addr,
                                    socklen_t addrlen,
                                    void *arg);

typedef void (*CellRudpContextEventHandler)(int ctx_id,
                                            int event_id,
                                            int error_code,
                                            void *arg);

#define CELL_RUDP_SUCCESS 0

#define CELL_RUDP_EVENT_SEND            1
#define CELL_RUDP_EVENT_SOCKET_RELEASED 2
#define CELL_RUDP_EVENT_DIAG_SENT       100
#define CELL_RUDP_EVENT_DIAG_RCVD       101

#define CELL_RUDP_CONTEXT_EVENT_CLOSED      1
#define CELL_RUDP_CONTEXT_EVENT_ESTABLISHED 2
#define CELL_RUDP_CONTEXT_EVENT_ERROR       3
#define CELL_RUDP_CONTEXT_EVENT_WRITABLE    4
#define CELL_RUDP_CONTEXT_EVENT_READABLE    5
#define CELL_RUDP_CONTEXT_EVENT_FLUSHED     6

#define CELL_RUDP_MUXMODE_P2P 1

#define CELL_RUDP_MSG_DONTWAIT          0x01
#define CELL_RUDP_MSG_LATENCY_CRITICAL  0x08
#define CELL_RUDP_MSG_ALIGN_32          0x10
#define CELL_RUDP_MSG_ALIGN_64          0x20
#define CELL_RUDP_MSG_WITH_TX_TIMESTAMP 0x40

#define CELL_RUDP_OPTION_MAX_PAYLOAD         1
#define CELL_RUDP_OPTION_SNDBUF              2
#define CELL_RUDP_OPTION_RCVBUF              3
#define CELL_RUDP_OPTION_NODELAY             4
#define CELL_RUDP_OPTION_DELIVERY_CRITICAL   5
#define CELL_RUDP_OPTION_ORDER_CRITICAL      6
#define CELL_RUDP_OPTION_NONBLOCK            7
#define CELL_RUDP_OPTION_STREAM              8
#define CELL_RUDP_OPTION_CONNECTION_TIMEOUT  9
#define CELL_RUDP_OPTION_CLOSE_WAIT_TIMEOUT 10
#define CELL_RUDP_OPTION_AGGREGATION_TIMEOUT 11
#define CELL_RUDP_OPTION_LAST_ERROR         14
#define CELL_RUDP_OPTION_READ_TIMEOUT       15
#define CELL_RUDP_OPTION_WRITE_TIMEOUT      16
#define CELL_RUDP_OPTION_FLUSH_TIMEOUT      17
#define CELL_RUDP_OPTION_KEEP_ALIVE_INTERVAL 18
#define CELL_RUDP_OPTION_KEEP_ALIVE_TIMEOUT  19

#define CELL_RUDP_STATE_IDLE        0
#define CELL_RUDP_STATE_CLOSED      1
#define CELL_RUDP_STATE_SYN_SENT    2
#define CELL_RUDP_STATE_SYN_RCVD    3
#define CELL_RUDP_STATE_ESTABLISHED 4
#define CELL_RUDP_STATE_CLOSE_WAIT  5

#define CELL_RUDP_POLL_OP_ADD    1
#define CELL_RUDP_POLL_OP_MODIFY 2
#define CELL_RUDP_POLL_OP_REMOVE 3

#define CELL_RUDP_POLL_EV_READ  0x0001
#define CELL_RUDP_POLL_EV_WRITE 0x0002
#define CELL_RUDP_POLL_EV_FLUSH 0x0004
#define CELL_RUDP_POLL_EV_ERROR 0x0008

#define CELL_RUDP_USEC_INDEFINITE 0xffffffffffffffffULL

#define CELL_RUDP_ERROR(_sts) (0x80000000U | (0x077U << 16) | (_sts))
#define CELL_RUDP_ERROR_NOT_INITIALIZED              CELL_RUDP_ERROR(1)
#define CELL_RUDP_ERROR_ALREADY_INITIALIZED          CELL_RUDP_ERROR(2)
#define CELL_RUDP_ERROR_INVALID_CONTEXT_ID           CELL_RUDP_ERROR(3)
#define CELL_RUDP_ERROR_INVALID_ARGUMENT             CELL_RUDP_ERROR(4)
#define CELL_RUDP_ERROR_INVALID_OPTION               CELL_RUDP_ERROR(5)
#define CELL_RUDP_ERROR_INVALID_MUXMODE              CELL_RUDP_ERROR(6)
#define CELL_RUDP_ERROR_MEMORY                       CELL_RUDP_ERROR(7)
#define CELL_RUDP_ERROR_INTERNAL                     CELL_RUDP_ERROR(8)
#define CELL_RUDP_ERROR_CONN_RESET                   CELL_RUDP_ERROR(9)
#define CELL_RUDP_ERROR_CONN_REFUSED                 CELL_RUDP_ERROR(10)
#define CELL_RUDP_ERROR_CONN_TIMEOUT                 CELL_RUDP_ERROR(11)
#define CELL_RUDP_ERROR_CONN_VERSION_MISMATCH        CELL_RUDP_ERROR(12)
#define CELL_RUDP_ERROR_CONN_TRANSPORT_TYPE_MISMATCH CELL_RUDP_ERROR(13)
#define CELL_RUDP_ERROR_CONN_QUALITY_LEVEL_MISMATCH  CELL_RUDP_ERROR(14)
#define CELL_RUDP_ERROR_THREAD                       CELL_RUDP_ERROR(15)
#define CELL_RUDP_ERROR_THREAD_IN_USE                CELL_RUDP_ERROR(16)
#define CELL_RUDP_ERROR_NOT_ACCEPTABLE               CELL_RUDP_ERROR(17)
#define CELL_RUDP_ERROR_MSG_TOO_LARGE                CELL_RUDP_ERROR(18)
#define CELL_RUDP_ERROR_NOT_BOUND                    CELL_RUDP_ERROR(19)
#define CELL_RUDP_ERROR_CANCELLED                    CELL_RUDP_ERROR(20)
#define CELL_RUDP_ERROR_INVALID_VPORT                CELL_RUDP_ERROR(21)
#define CELL_RUDP_ERROR_WOULDBLOCK                   CELL_RUDP_ERROR(22)
#define CELL_RUDP_ERROR_VPORT_IN_USE                 CELL_RUDP_ERROR(23)
#define CELL_RUDP_ERROR_VPORT_EXHAUSTED              CELL_RUDP_ERROR(24)
#define CELL_RUDP_ERROR_INVALID_SOCKET               CELL_RUDP_ERROR(25)
#define CELL_RUDP_ERROR_BUFFER_TOO_SMALL             CELL_RUDP_ERROR(26)
#define CELL_RUDP_ERROR_MSG_MALFORMED                CELL_RUDP_ERROR(27)
#define CELL_RUDP_ERROR_ADDR_IN_USE                  CELL_RUDP_ERROR(28)
#define CELL_RUDP_ERROR_ALREADY_BOUND                CELL_RUDP_ERROR(29)
#define CELL_RUDP_ERROR_ALREADY_EXISTS               CELL_RUDP_ERROR(30)
#define CELL_RUDP_ERROR_INVALID_POLL_ID              CELL_RUDP_ERROR(31)
#define CELL_RUDP_ERROR_TOO_MANY_CONTEXTS            CELL_RUDP_ERROR(32)
#define CELL_RUDP_ERROR_IN_PROGRESS                  CELL_RUDP_ERROR(33)
#define CELL_RUDP_ERROR_NO_EVENT_HANDLER             CELL_RUDP_ERROR(34)
#define CELL_RUDP_ERROR_PAYLOAD_TOO_LARGE            CELL_RUDP_ERROR(35)
#define CELL_RUDP_ERROR_END_OF_DATA                  CELL_RUDP_ERROR(36)
#define CELL_RUDP_ERROR_ALREADY_ESTABLISHED          CELL_RUDP_ERROR(37)
#define CELL_RUDP_ERROR_KEEP_ALIVE_FAILURE           CELL_RUDP_ERROR(38)

typedef struct CellRudpReadInfo {
    uint8_t size;
    uint8_t retransmissionCount;
    uint16_t retransmissionDelay;
    uint8_t retransmissionDelay2;
    uint8_t flags;
    uint16_t sequenceNumber;
    uint32_t timestamp;
} CellRudpReadInfo;

typedef struct CellRudpContextStatus {
    uint32_t state;
    int parentId;
    uint32_t children;
    uint32_t lostPackets;
    uint32_t sentPackets;
    uint32_t rcvdPackets;
    uint64_t sentBytes;
    uint64_t rcvdBytes;
    uint32_t retransmissions;
    uint32_t rtt;
} CellRudpContextStatus;

typedef struct CellRudpPollEvent {
    int ctx_id;
    uint16_t reqevents;
    uint16_t rtnevents;
} CellRudpPollEvent;

typedef struct CellRudpStatus {
    uint32_t sentUdpPackets;
    uint64_t sentUdpBytes;
    uint32_t rcvdUdpPackets;
    uint64_t rcvdUdpBytes;
    uint32_t sentUserPackets;
    uint64_t sentUserBytes;
    uint32_t sentLatencyCriticalPackets;
    uint32_t rcvdUserPackets;
    uint64_t rcvdUserBytes;
    uint32_t rcvdLatencyCriticalPackets;
    uint32_t sentSynPackets;
    uint32_t rcvdSynPackets;
    uint32_t sentUsrPackets;
    uint32_t rcvdUsrPackets;
    uint32_t sentPrbPackets;
    uint32_t rcvdPrbPackets;
    uint32_t sentRstPackets;
    uint32_t rcvdRstPackets;
    uint32_t lostPackets;
    uint32_t retransmittedPackets;
    uint32_t reorderedPackets;
    uint32_t sentQualityLevel1Packets;
    uint64_t sentQualityLevel1Bytes;
    uint32_t rcvdQualityLevel1Packets;
    uint64_t rcvdQualityLevel1Bytes;
    uint32_t sentQualityLevel2Packets;
    uint64_t sentQualityLevel2Bytes;
    uint32_t rcvdQualityLevel2Packets;
    uint64_t rcvdQualityLevel2Bytes;
    uint32_t sentQualityLevel3Packets;
    uint64_t sentQualityLevel3Bytes;
    uint32_t rcvdQualityLevel3Packets;
    uint64_t rcvdQualityLevel3Bytes;
    uint32_t sentQualityLevel4Packets;
    uint64_t sentQualityLevel4Bytes;
    uint32_t rcvdQualityLevel4Packets;
    uint64_t rcvdQualityLevel4Bytes;
    uint32_t currentContexts;
    uint32_t allocs;
    uint32_t frees;
    uint32_t memCurrent;
    uint32_t memPeak;
    uint32_t establishedConnections;
    uint32_t failedConnections;
    uint32_t failedConnectionsReset;
    uint32_t failedConnectionsRefused;
    uint32_t failedConnectionsTimeout;
    uint32_t failedConnectionsVersionMismatch;
    uint32_t failedConnectionsTransportTypeMismatch;
    uint32_t failedConnectionsQualityLevelMismatch;
} CellRudpStatus;

int cellRudpInit(CellRudpAllocator *allocator);
int cellRudpEnd(void);
int cellRudpEnableInternalIOThread(uint32_t stackSize, uint32_t priority);
int cellRudpSetEventHandler(CellRudpEventHandler handler, void *arg);
int cellRudpSetMaxSegmentSize(uint16_t mss);
int cellRudpGetMaxSegmentSize(uint16_t *mss);
int cellRudpProcessEvents(CellRudpUsec timeout);
int cellRudpCreateContext(CellRudpContextEventHandler handler, void *arg, int *ctx_id);
int cellRudpBind(int ctx_id, int soc, uint16_t vport, uint8_t mux_mode);
int cellRudpActivate(int ctx_id, const struct sockaddr *to, socklen_t tolen);
int cellRudpInitiate(int ctx_id, const struct sockaddr *to, socklen_t tolen, uint16_t vport);
int cellRudpTerminate(int ctx_id);
int cellRudpWrite(int ctx_id, const void *data, size_t len, uint8_t flags);
int cellRudpRead(int ctx_id, void *data, size_t len, uint8_t flags, CellRudpReadInfo *info);
ssize_t cellRudpGetSizeWritable(int ctx_id);
ssize_t cellRudpGetSizeReadable(int ctx_id);
int cellRudpFlush(int ctx_id);
int cellRudpNetReceived(int soc, const uint8_t *data, size_t datalen, const struct sockaddr *from, socklen_t fromlen);
int cellRudpGetLocalInfo(int ctx_id, int *soc, struct sockaddr *addr, socklen_t *addrlen, uint16_t *vport, uint8_t *mux_mode);
int cellRudpGetRemoteInfo(int ctx_id, struct sockaddr *addr, socklen_t *addrlen, uint16_t *vport);
int cellRudpGetContextStatus(int ctx_id, CellRudpContextStatus *status, size_t statusSize);
int cellRudpSetOption(int ctx_id, int option, const void *optval, size_t optlen);
int cellRudpGetOption(int ctx_id, int option, void *optval, size_t optlen);
int cellRudpPollCreate(size_t size);
int cellRudpPollDestroy(int poll_id);
int cellRudpPollControl(int poll_id, int op, int ctx_id, uint16_t events);
int cellRudpPollWait(int poll_id, CellRudpPollEvent *events, size_t eventlen, CellRudpUsec timeout);
int cellRudpPollCancel(int poll_id);
int cellRudpGetStatus(CellRudpStatus *status, size_t statusSize);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_RUDP_H__ */
