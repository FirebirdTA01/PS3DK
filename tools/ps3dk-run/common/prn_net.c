/* ps3dk-run: blocking socket helpers with per-call deadlines. */
#include "prn_net.h"
#include "ps3dk_run_proto.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>


int prn_set_timeouts(int fd, int recv_s, int send_s)
{
    struct timeval r = { recv_s, 0 }, s = { send_s, 0 };
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &r, sizeof r) != 0) return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &s, sizeof s) != 0) return -1;
    return 0;
}

int prn_send_all(int fd, const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t k = send(fd, c, n, 0);
        if (k <= 0) return -1;
        c += k; n -= (size_t)k;
    }
    return 0;
}

int (*prn_stop_check)(void);
int prn_stop_idle_ms = 30000;

/* With prn_stop_check set, wait for data in slices of PRN_STOP_SLICE_MS: fail on a stop request, or once
   prn_stop_idle_ms pass without data (the no-progress deadline). */
static int wait_data(int fd)
{
    int idle = 0;
    for (;;) {
        struct timeval tv = { 0, PRN_STOP_SLICE_MS * 1000 };
        fd_set r;
        int n;
        if (prn_stop_check()) return -1;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        n = select(fd + 1, &r, NULL, NULL, &tv);
        if (n < 0) return -1;
        if (n > 0 && FD_ISSET(fd, &r)) return 0;
        idle += PRN_STOP_SLICE_MS;
        if (idle >= prn_stop_idle_ms) return -1;
    }
}

int prn_recv_all(int fd, void *p, size_t n)
{
    char *c = p;
    while (n) {
        if (prn_stop_check && wait_data(fd) != 0) return -1;
        ssize_t k = recv(fd, c, n, 0);
        if (k <= 0) return -1;
        c += k; n -= (size_t)k;
    }
    return 0;
}

int prn_send_frame(int fd, uint8_t type, const void *payload, uint32_t len)
{
    uint8_t h[9];
    prn_put32(h, PRN_MAGIC);
    h[4] = type;
    prn_put32(h + 5, len);
    if (prn_send_all(fd, h, sizeof h) != 0) return -1;
    return len ? prn_send_all(fd, payload, len) : 0;
}

int prn_recv_header(int fd, uint8_t *type, uint32_t *len)
{
    uint8_t h[9];
    if (prn_recv_all(fd, h, sizeof h) != 0) return -1;
    if (prn_get32(h) != PRN_MAGIC) return -1;
    *type = h[4];
    *len = prn_get32(h + 5);
    return *len > PRN_MAX_FRAME ? -1 : 0;
}

int prn_connect(const char *ip, uint16_t port, int timeout_s)
{
    struct sockaddr_in a;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    prn_set_timeouts(fd, timeout_s, timeout_s);
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr(ip);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { socketclose(fd); return -1; }
    return fd;
}

#include "prn_hmac.h"

void prn_tag(const uint8_t key[32], uint8_t dir, uint32_t seq, uint8_t type, const uint8_t *p, uint32_t n,
             uint8_t out[32])
{
    uint8_t h[10];
    h[0] = dir;
    prn_put32(h + 1, seq);
    h[5] = type;
    prn_put32(h + 6, n);
    prn_hmac4(key, h, sizeof h, p, n, NULL, 0, NULL, 0, out);
}

int prn_chan_send(prn_chan *c, uint8_t type, const void *p, uint32_t n)
{
    uint8_t tag[PRN_TAG];
    prn_tag(c->key, c->out_dir, c->out_seq, type, p, n, tag);
    if (prn_send_frame(c->fd, type, p, n) != 0) return -1;
    if (prn_send_all(c->fd, tag, sizeof tag) != 0) return -1;
    c->out_seq++;
    return 0;
}

int prn_chan_recv(prn_chan *c, uint8_t *type, uint8_t *buf, uint32_t cap, uint32_t *n)
{
    uint8_t tag[PRN_TAG], want[PRN_TAG];
    if (prn_recv_header(c->fd, type, n) != 0 || *n > cap) return -1;
    if (*n && prn_recv_all(c->fd, buf, *n) != 0) return -1;
    if (prn_recv_all(c->fd, tag, sizeof tag) != 0) return -1;
    prn_tag(c->key, c->in_dir, c->in_seq, *type, buf, *n, want);
    if (prn_tag_diff(tag, want)) return -1;
    c->in_seq++;
    return 0;
}
