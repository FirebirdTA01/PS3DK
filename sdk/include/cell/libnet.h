#ifndef __PS3DK_CELL_LIBNET_H__
#define __PS3DK_CELL_LIBNET_H__

#include <stdint.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <ppu-types.h>

#ifndef ATTRIBUTE_PRXPTR
#define ATTRIBUTE_PRXPTR
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t sys_net_thread_id_t;

typedef struct sys_net_initialize_parameter {
    void *memory ATTRIBUTE_PRXPTR;
    uint32_t memory_size;
    int32_t flags;
} sys_net_initialize_parameter_t;

typedef struct sys_net_test_param_ops {
    uint16_t drop_rate;
    uint16_t drop_duration;
    uint16_t pass_duration;
    uint16_t delay_time;
    uint16_t delay_jitter;
    uint16_t order_rate;
    uint16_t order_delay_time;
    uint16_t duplication_rate;
    uint32_t bps_limit;
    uint16_t lower_size_limit;
    uint16_t upper_size_limit;
    uint64_t policy_pattern;
    uint16_t policy_flags[64];
    uint8_t reserved[64];
} sys_net_test_param_ops_t;

typedef struct sys_net_test_param {
    uint16_t version;
    uint16_t option_number;
    uint16_t current_version;
    uint16_t result;
    uint32_t flags;
    uint32_t reserved2;
    sys_net_test_param_ops_t send;
    sys_net_test_param_ops_t recv;
    uint32_t seed;
    uint8_t reserved[44];
} sys_net_test_param_t;

typedef struct sys_net_sockinfo {
    int32_t s;
    int32_t proto;
    int32_t recv_queue_length;
    int32_t send_queue_length;
    struct in_addr local_adr;
    int32_t local_port;
    struct in_addr remote_adr;
    int32_t remote_port;
    int32_t state;
} sys_net_sockinfo_t;

typedef struct sys_net_sockinfo_ex {
    int32_t s;
    int32_t proto;
    int32_t recv_queue_length;
    int32_t send_queue_length;
    struct in_addr local_adr;
    int32_t local_port;
    struct in_addr remote_adr;
    int32_t remote_port;
    int32_t state;
    int32_t socket_type;
    int32_t local_vport;
    int32_t remote_vport;
    int32_t reserved[8];
} sys_net_sockinfo_ex_t;

typedef unsigned int nfds_t;

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLRDNORM 0x0040
#define POLLWRNORM POLLOUT
#define POLLRDBAND 0x0080
#define POLLWRBAND 0x0100

#define SYS_NET_INIT_ERROR_CHECK 0x0001

#define SYS_NET_ABORT_STRICT_CHECK 0x0001

#define SYS_NET_DUMP_TCPDUMP  0x0000
#define SYS_NET_DUMP_SYSLOG   0x0001
#define SYS_NET_DUMP_PEEK     0x0010
#define SYS_NET_DUMP_DONTWAIT 0x0020
#define SYS_NET_DUMP_OVERFLOW 0x0040

#define SYS_NET_THREAD_SELF           0x0001
#define SYS_NET_THREAD_ALL            0x0002
#define SYS_NET_THREAD_ABORT_RESOLVER 0x0004

#define SYS_NET_STATE_UNKNOWN      0
#define SYS_NET_STATE_CLOSED       1
#define SYS_NET_STATE_CREATED      2
#define SYS_NET_STATE_OPENED       3
#define SYS_NET_STATE_LISTEN       4
#define SYS_NET_STATE_SYN_SENT     5
#define SYS_NET_STATE_SYN_RECEIVED 6
#define SYS_NET_STATE_ESTABLISHED  7
#define SYS_NET_STATE_FIN_WAIT_1   8
#define SYS_NET_STATE_FIN_WAIT_2   9
#define SYS_NET_STATE_CLOSE_WAIT   10
#define SYS_NET_STATE_CLOSING      11
#define SYS_NET_STATE_LAST_ACK     12
#define SYS_NET_STATE_TIME_WAIT    13

#define SYS_NET_SOCKINFO_EX_USER_ONLY 0x00000001
#define SYS_NET_SOCKINFO_EX_PCBTABLES 0x00000002

#define SYS_NET_CC_GET_MEMORY_FREE_CURRENT     0x00000280
#define SYS_NET_CC_GET_MEMORY_FREE_MINIMUM     0x00000281
#define SYS_NET_CC_GET_LIB_MEMORY_FREE_CURRENT 0x00000282
#define SYS_NET_CC_GET_LIB_MEMORY_FREE_MINIMUM 0x00000283
#define SYS_NET_CC_CLEAR_DNS_CACHE             0x00000290

#define sys_net_errno (*_sys_net_errno_loc())

#define sys_net_initialize_network() ({                         \
    static uint8_t __libnet_memory[128 * 1024];                  \
    sys_net_initialize_parameter_t __libnet_param;               \
    __libnet_param.memory = __libnet_memory;                     \
    __libnet_param.memory_size = (uint32_t)sizeof(__libnet_memory); \
    __libnet_param.flags = 0;                                    \
    sys_net_initialize_network_ex(&__libnet_param);              \
})

int *_sys_net_errno_loc(void);
int *_sys_net_h_errno_loc(void);

int socketpoll(struct pollfd *fds, nfds_t nfds, int timeout);
int socketselect(int nfds, fd_set *readfds, fd_set *writefds,
                 fd_set *exceptfds, struct timeval *timeout);

int sys_net_initialize_network_ex(sys_net_initialize_parameter_t *param);
int sys_net_finalize_network(void);
int sys_net_abort_socket(int sockfd, int flags);
int sys_net_abort_resolver(sys_net_thread_id_t tid, int flags);
int sys_net_open_dump(int len, int flags);
int sys_net_read_dump(int id, void *buf, int len, int *pflags);
int sys_net_close_dump(int id, int *pflags);
int sys_net_set_resolver_configurations(int retrans, int retry, int flags);
int sys_net_free_thread_context(sys_net_thread_id_t tid, int flags);
int sys_net_show_ifconfig(void);
int sys_net_show_nameserver(void);
int sys_net_show_route(void);
int sys_net_set_netemu_test_param(sys_net_test_param_t *param);
int sys_net_get_netemu_test_param(sys_net_test_param_t *param);
int sys_net_get_lib_name_server(struct in_addr *primary,
                                struct in_addr *secondary);
int sys_net_set_lib_name_server(struct in_addr *primary,
                                struct in_addr *secondary);
int sys_net_get_sockinfo(int s, sys_net_sockinfo_t *p, int n);
int sys_net_get_sockinfo_ex(int s, sys_net_sockinfo_ex_t *p, int n,
                            int flags);
int sys_net_get_test_param(sys_net_test_param_t *param);
int sys_net_set_test_param(sys_net_test_param_t *param);
int sys_net_get_udpp2p_test_param(sys_net_test_param_t *param);
int sys_net_set_udpp2p_test_param(sys_net_test_param_t *param);
int sys_net_if_ctl(int if_id, int code, void *ptr, int len);

#ifdef __cplusplus
}
#endif

#endif /* __PS3DK_CELL_LIBNET_H__ */
