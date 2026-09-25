/* Real syscall loopback gate; DNS/recvmsg require separate firmware coverage. */
#include <sys/process.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/lv2_syscall.h>
#include <net/net.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
SYS_PROCESS_PARAM(1001, 0x10000);
#define CHECK(x) do { if (!(x)) { printf("LIBNET_FAIL line=%d errno=%d: %s\n", __LINE__, errno, #x); return 1; } } while (0)

/* Only corroborate an observed SDK EOPNOTSUPP, never substitute for its call. */
static int raw_waitall_error(int s, void *buffer, unsigned size)
{
    lv2syscall6(707, s & ~SOCKET_FD_MASK, (uint64_t)buffer, size, MSG_WAITALL, 0, 0);
    return_to_user_prog(int);
}

static int timeouts(int s, int expected_type)
{
    struct timeval tv = {2, 500};
    socklen_t len = sizeof(tv);
    CHECK(setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    CHECK(setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0);
    memset(&tv, 0, sizeof(tv));
    errno = 0;
    int rc = getsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, &len);
    printf("LIBNET_TIMEOUT fd=%d rc=%d errno=%d len=%u sec=%lld usec=%lld\n",
           s, rc, errno, (unsigned)len, (long long)tv.tv_sec, (long long)tv.tv_usec);
    CHECK(rc == 0);
    CHECK(len == sizeof(tv));
    if (tv.tv_sec == 2000 && tv.tv_usec == 0) {
        /* Observed in both ABIs in r2. Windows RPCS3 stores a DWORD timeout
         * then reads it as timeval: lv2_socket_native.cpp:595-601/721-725.
         * This is a reported emulator limitation, not timeout readback PASS. */
        printf("LIBNET_EMULATOR_LIMIT timeout_readback expected=2s500us observed=2000s0us\n");
    } else {
        CHECK(tv.tv_sec == 2 && tv.tv_usec >= 0 && tv.tv_usec < 1000000);
        printf("LIBNET_TIMEOUT_READBACK_OK\n");
    }
    int value = -1; len = sizeof(value);
    errno = 0;
    rc = getsockopt(s, SOL_SOCKET, SO_TYPE, &value, &len);
    int type_error = errno;
    printf("LIBNET_TYPE fd=%d rc=%d errno=%d len=%u value=%d\n",
           s, rc, type_error, (unsigned)len, value);
    if (rc == -1 && type_error == EINVAL) {
        /* RPCS3 lv2_socket_native.cpp:388-479 has no SO_TYPE case. */
        printf("LIBNET_EMULATOR_UNIMPLEMENTED SO_TYPE errno=EINVAL\n");
    } else {
        CHECK(rc == 0 && len == sizeof(value) && value == expected_type);
    }
    value = 1;
    CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value)) == 0);
    value = 0; len = sizeof(value);
    CHECK(getsockopt(s, SOL_SOCKET, SO_REUSEADDR, &value, &len) == 0);
    CHECK(len == sizeof(value) && value != 0);
    value = -1; len = sizeof(value);
    CHECK(getsockopt(s, SOL_SOCKET, SO_ERROR, &value, &len) == 0);
    CHECK(len == sizeof(value) && value == 0);
    return 0;
}
static int ready(int s)
{
    struct pollfd p = {s, POLLIN, 0};
    CHECK(socketpoll(&p, 1, 2000) == 1 && (p.revents & POLLIN));
    CHECK(p.fd == s);
    fd_set fds;
    FD_ZERO(&fds); FD_SET(s, &fds);
    struct timeval t = {2, 0};
    CHECK(socketselect(s + 1, &fds, NULL, NULL, &t) == 1 && FD_ISSET(s, &fds));
    return 0;
}
int main(void)
{
    printf("LIBNET_BEGIN pointer=%u\n", (unsigned)sizeof(void *));
    printf("LIBNET_EMULATOR_UNIMPLEMENTED firmware_DNS recvmsg sockinfo (host-boundary only)\n");
    printf("LIBNET_EMULATOR_LIMIT module/network lifecycle=HLE-noop (host controls only)\n");
    CHECK(netInitialize() == 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = sizeof(addr); addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    int server = socket(AF_INET, SOCK_DGRAM, 0);
    int client = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(server >= 0 && client >= 0 && (server & SOCKET_FD_MASK) && (client & SOCKET_FD_MASK));
    int server_timeout = timeouts(server, SOCK_DGRAM);
    int client_timeout = timeouts(client, SOCK_DGRAM);
    printf("LIBNET_TIMEOUT_RESULTS server=%d client=%d\n", server_timeout, client_timeout);
    CHECK(server_timeout == 0 && client_timeout == 0);
    CHECK(bind(server, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    socklen_t len = sizeof(addr);
    CHECK(getsockname(server, (struct sockaddr *)&addr, &len) == 0 && addr.sin_port != 0);
    const char packet[] = "PS3DK coherent sockets";
    CHECK(sendto(client, packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr)) == sizeof(packet));
    CHECK(ready(server) == 0);
    char buffer[64] = {0};
    CHECK(recvfrom(server, buffer, sizeof(buffer), 0, NULL, NULL) == sizeof(packet));
    CHECK(!memcmp(buffer, packet, sizeof(packet)));
    CHECK(recv(server, buffer, sizeof(buffer), MSG_DONTWAIT) == -1 && errno == EAGAIN);
    CHECK(close(client) == 0 && close(server) == 0);
    CHECK(socketclose(server) == -1 && errno == EBADF);
    printf("LIBNET_UDP_OK\n");

    addr.sin_port = 0;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(listener >= 0 && client >= 0);
    CHECK(timeouts(listener, SOCK_STREAM) == 0 && timeouts(client, SOCK_STREAM) == 0);
    CHECK(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(listener, 1) == 0);
    len = sizeof(addr);
    CHECK(getsockname(listener, (struct sockaddr *)&addr, &len) == 0);
    CHECK(connect(client, (struct sockaddr *)&addr, len) == 0);
    CHECK(ready(listener) == 0);
    server = accept(listener, NULL, NULL);
    CHECK(server >= 0 && (server & SOCKET_FD_MASK) && timeouts(server, SOCK_STREAM) == 0);
    CHECK(write(client, packet, sizeof(packet)) == sizeof(packet));
    CHECK(ready(server) == 0);
    memset(buffer, 0, sizeof(buffer));
    CHECK(read(server, buffer, sizeof(packet)) == sizeof(packet) && !memcmp(buffer, packet, sizeof(packet)));
    CHECK(send(server, packet, sizeof(packet), 0) == sizeof(packet));
    CHECK(ready(client) == 0);
    errno = 0;
    ssize_t received = recv(client, buffer, sizeof(packet), MSG_WAITALL);
    int receive_error = errno;
    printf("LIBNET_WAITALL rc=%lld errno=%d expected_native_EOPNOTSUPP=%d\n",
           (long long)received, receive_error, EOPNOTSUPP);
    if (received == -1 && receive_error == EOPNOTSUPP) {
        int raw = raw_waitall_error(client, buffer, sizeof(packet));
        printf("LIBNET_WAITALL_ERROR raw=%d translated=%d\n", raw, receive_error);
        CHECK(raw == -NET_EOPNOTSUPP);
        printf("LIBNET_EMULATOR_LIMIT nonblocking_Windows_MSG_WAITALL\n");
        size_t total = 0;
        unsigned attempts;
        for (attempts = 0; total < sizeof(packet) && attempts < sizeof(packet); ++attempts) {
            received = recv(client, buffer + total, sizeof(packet) - total, 0);
            CHECK(received > 0 && (size_t)received <= sizeof(packet) - total);
            total += (size_t)received;
        }
        CHECK(total == sizeof(packet));
    } else {
        CHECK(received == sizeof(packet));
    }
    CHECK(!memcmp(buffer, packet, sizeof(packet)));
    int error = -1; len = sizeof(error);
    CHECK(getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0);
    CHECK(close(server) == 0 && close(client) == 0 && close(listener) == 0);
    printf("LIBNET_TCP_OK\n");
    CHECK(netDeinitialize() == 0);
    printf("LIBNET_OK pointer=%u (DNS/recvmsg excluded)\n", (unsigned)sizeof(void *));
    return 0;
}
