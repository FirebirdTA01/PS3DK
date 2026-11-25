/* ps3dk-run: blocking socket helpers with per-call deadlines (SO_RCVTIMEO / SO_SNDTIMEO). */
#ifndef PRN_NET_H
#define PRN_NET_H

#include <stddef.h>
#include <stdint.h>

/* All return 0 on success, -1 on error, timeout or peer close.  Partial transfers are retried. */
int prn_set_timeouts(int fd, int recv_s, int send_s);
int prn_send_all(int fd, const void *p, size_t n);
/* Optional stop check for receives (the loader sets it to loader_stopping; NULL = plain blocking receives, as in the
   run hook).  When set, receives wait in PRN_STOP_SLICE_MS slices and give up after prn_stop_idle_ms without data. */
#define PRN_STOP_SLICE_MS 250
extern int (*prn_stop_check)(void);
extern int prn_stop_idle_ms;
int prn_recv_all(int fd, void *p, size_t n);
/* Frame header + payload in one call (the payload may be NULL when len is 0). */
int prn_send_frame(int fd, uint8_t type, const void *payload, uint32_t len);
/* Reads a frame header; *len is checked against PRN_MAX_FRAME.  The payload is left on the socket. */
int prn_recv_header(int fd, uint8_t *type, uint32_t *len);
/* Connects to a dotted IPv4 address; returns the socket or -1. */
int prn_connect(const char *ip, uint16_t port, int timeout_s);


/* ---- authenticated channel (protocol 2) ---- */
#define PRN_CHAN_DEFINED 1
typedef struct {
    int fd;
    uint8_t key[32];          /* session key Ks or run key Kr */
    uint8_t out_dir, in_dir;  /* direction bytes bound into the tags */
    uint32_t out_seq, in_seq; /* next sequence number each way */
} prn_chan;

/* tag = HMAC(key, dir | seq | type | len | payload) */
void prn_tag(const uint8_t key[32], uint8_t dir, uint32_t seq, uint8_t type, const uint8_t *p, uint32_t n,
             uint8_t out[32]);
/* header, payload and tag in one send; advances out_seq */
int prn_chan_send(prn_chan *c, uint8_t type, const void *p, uint32_t n);
/* one whole frame into buf (cap bytes); verifies magic, length, tag and that seq is exactly the next one; advances
 * in_seq.  -1 on any failure (the caller closes the connection). */
int prn_chan_recv(prn_chan *c, uint8_t *type, uint8_t *buf, uint32_t cap, uint32_t *n);

#endif
