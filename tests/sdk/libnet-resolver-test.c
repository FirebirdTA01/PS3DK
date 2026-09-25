#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <netdb.h>
#include <net/net.h>
#include <cell/libnet.h>
#include <arpa/inet.h>
extern struct hostent *gethostbyname(const char *) __attribute__((weak));
extern struct hostent *gethostbyaddr(const char *, socklen_t, int) __attribute__((weak));
extern struct servent *getservbyport(int, const char *) __attribute__((weak));
extern int sys_net_initialize_network_ex(sys_net_initialize_parameter_t *) __attribute__((weak));
extern int netInitialize(void) __attribute__((weak));
extern int netDeinitialize(void) __attribute__((weak));
extern int netClose(int) __attribute__((weak));
extern int sys_net_abort_socket(int, int) __attribute__((weak));
extern int sys_net_get_sockinfo(int, sys_net_sockinfo_t *, int) __attribute__((weak));
extern int sys_net_get_sockinfo_ex(int, sys_net_sockinfo_ex_t *, int, int) __attribute__((weak));
extern int netGetSockInfo(int, netSocketInfo *, int) __attribute__((weak));
static int info_fd, info_result = 2;
int __ps3dk_raw_sys_net_abort_socket(int s, int f) { info_fd = s; return f; }
int __ps3dk_raw_sys_net_get_sockinfo(int s, void *ptr, int n)
{ info_fd = s; if (n >= 2 && info_result > 0) { sys_net_sockinfo_t *p = ptr; p[0].s = 3; p[1].s = -1; } return info_result; }
int __ps3dk_raw_sys_net_get_sockinfo_ex(int s, void *ptr, int n, int f)
{ (void)f; info_fd = s; if (n >= 2 && info_result > 0) { sys_net_sockinfo_ex_t *p = ptr; p[0].s = 3; p[1].s = -1; } return info_result; }
static int failures, herr, neterr, null_result, legacy_init, init_calls, final_calls;
static int load_calls, unload_calls, load_result, init_result;
int cellSysmoduleLoadModule(uint16_t id) { ++load_calls; if (id != 0) return -1; return load_result; }
int cellSysmoduleUnloadModule(uint16_t id) { ++unload_calls; return id != 0 ? -1 : 0; }
int sys_net_finalize_network(void) { ++final_calls; return 0; }
int __ps3dk_raw_socketclose(int fd) { (void)fd; return 0; }
static char hostname[] = "fixture.example", alias[] = "alias.example";
static uint8_t address[] = {127, 0, 0, 1};
static uint32_t aliases[2], addresses[2], host[5];
static unsigned char memory[4096];
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "resolver FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
int *_sys_net_h_errno_loc(void) { return &herr; }
int *_sys_net_errno_loc(void) { return &neterr; }
uint32_t __ps3dk_raw_gethostbyname(const char *name)
{ (void)name; return null_result ? 0 : (uint32_t)(uintptr_t)host; }
uint32_t __ps3dk_raw_gethostbyaddr(const char *addr, socklen_t len, int type)
{ CHECK(addr == (char *)address && len == 4 && type == AF_INET); return (uint32_t)(uintptr_t)host; }
int __ps3dk_raw_sys_net_initialize_network_ex(const uint32_t *p)
{ ++init_calls; if (legacy_init) { CHECK(p[0] && p[1] == 128*1024 && p[2] == 0); }
  else { CHECK(p[0] == (uintptr_t)memory && p[1] == sizeof(memory) && p[2] == 1); } return init_result; }
int main(void)
{
    aliases[0] = (uintptr_t)alias; addresses[0] = (uintptr_t)address;
    host[0] = (uintptr_t)hostname; host[1] = (uintptr_t)aliases;
    host[2] = AF_INET; host[3] = 4; host[4] = (uintptr_t)addresses;
    CHECK(gethostbyname && gethostbyaddr && getservbyport && sys_net_initialize_network_ex);
    if (!gethostbyname || !gethostbyaddr || !getservbyport || !sys_net_initialize_network_ex) return 1;
    struct hostent *h = gethostbyname("fixture.example");
    CHECK(h && !strcmp(h->h_name, hostname));
    if (!h) return 1;
    CHECK(h->h_addrtype == AF_INET && h->h_length == 4);
    CHECK(h->h_aliases[1] == NULL && !strcmp(h->h_aliases[0], alias));
    CHECK(h->h_addr_list[1] == NULL && !memcmp(h->h_addr, address, 4));
    hostname[0] = 'X'; address[0] = 1;
    CHECK(h->h_name[0] == 'f' && (unsigned char)h->h_addr[0] == 127);
    h = gethostbyaddr((char *)address, 4, AF_INET);
    CHECK(h && h->h_name[0] == 'X' && h->h_addr[0] == 1);
    null_result = 1; herr = TRY_AGAIN; neterr = 35;
    CHECK(!gethostbyname("failure") && h_errno == TRY_AGAIN && errno == EAGAIN);
    struct servent *s = getservbyport(htons(80), "tcp");
    CHECK(s && !strcmp(s->s_name, "http") && !strcmp(s->s_proto, "tcp") && s->s_port == htons(80) && s->s_aliases[0] == NULL);
    CHECK(!getservbyport(htons(80), "udp"));
    sys_net_initialize_parameter_t param = {memory, sizeof(memory), 1};
    CHECK(sys_net_initialize_network_ex(&param) == 0);
    CHECK(load_calls == 0 && unload_calls == 0);
    CHECK(netInitialize && netDeinitialize && netClose);
    if (!netInitialize || !netDeinitialize || !netClose) return 1;
    legacy_init = 1; init_calls = 0;
    load_result = -7;
    CHECK(netInitialize() == -7 && init_calls == 0 && load_calls == 1);
    load_result = 0; init_result = -8;
    CHECK(netInitialize() == -8 && init_calls == 1 && unload_calls == 1);
    init_result = 0;
    CHECK(netInitialize() == 0 && netInitialize() == 0 && init_calls == 2 && load_calls == 3);
    CHECK(netDeinitialize() == 0 && final_calls == 1 && unload_calls == 2);
    CHECK(netDeinitialize() == 0 && final_calls == 1 && unload_calls == 2);
    CHECK(netClose(SOCKET_FD_MASK | 3) == -1 && neterr == 9);
    CHECK(sys_net_abort_socket && sys_net_get_sockinfo && sys_net_get_sockinfo_ex && netGetSockInfo);
    if (!sys_net_abort_socket || !sys_net_get_sockinfo || !sys_net_get_sockinfo_ex || !netGetSockInfo) return 1;
    sys_net_sockinfo_t info[3] = {0}; info[2].s = 77;
    CHECK(sys_net_get_sockinfo(-1, info, 3) == 2 && info_fd == -1);
    CHECK(info[0].s == (SOCKET_FD_MASK | 3) && info[1].s == -1 && info[2].s == 77);
    CHECK(sys_net_get_sockinfo(SOCKET_FD_MASK | 7, info, 3) == 2 && info_fd == 7);
    info_result = -1; info[0].s = 88; neterr = 61;
    CHECK(sys_net_get_sockinfo(-2, info, 3) == -1 && info_fd == -2 && info[0].s == 88 && neterr == 61);
    info_result = 2;
    sys_net_sockinfo_ex_t extended[3] = {0}; extended[2].s = 77;
    CHECK(sys_net_get_sockinfo_ex(-1, extended, 3, 2) == 2 && info_fd == -1);
    CHECK(extended[0].s == (SOCKET_FD_MASK | 3) && extended[1].s == -1 && extended[2].s == 77);
    CHECK(sys_net_abort_socket(SOCKET_FD_MASK | 7, -9) == -9 && info_fd == 7);
    CHECK(sys_net_abort_socket(-1, -9) == -9 && info_fd == -1);
    netSocketInfo raw_info[3] = {0}; raw_info[2].s = 77;
    CHECK(netGetSockInfo(-1, raw_info, 3) == 2 && info_fd == -1 && raw_info[0].s == 3 && raw_info[1].s == -1 && raw_info[2].s == 77);
    CHECK(netGetSockInfo(SOCKET_FD_MASK | 7, raw_info, 3) == -1 && neterr == NET_EBADF);
    if (!failures) printf("LIBNET_BOUNDARY_OK pointer=%u\n", (unsigned)sizeof(void *));
    return failures ? 1 : 0;
}
