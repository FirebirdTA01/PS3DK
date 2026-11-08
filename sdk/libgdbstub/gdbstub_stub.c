/* gdbstub_stub.c - not-implemented (NIMPL) implementations of the session
 * and codec entry points, for the RED baseline of t_d0968566.
 *
 * Step/status and codec entry points return NIMPL; state returns its out-of-set
 * sentinel and reason is NULL. There is no framing, state-machine dispatch,
 * validation or backend invocation. Codec calls initialize the reported size
 * to zero. create/free allocate/free only the inert handle below. Layout
 * queries live separately; pass/fail totals are measured by the runner.
 *
 * The version identifier lives here (not in the public header). The only
 * permitted scaffolding is an inert, correctly-sized session handle so
 * create/free round-trip; it carries no state.
 */
#include "gdbstub.h"
#include <stdlib.h>

#define GDBSTUB_STUB_VERSION "0.1-red-stub"

struct gdbstub_session {
  size_t reserved; /* inert sizing only; no behavioral state */
};

gdbstub_session *gdbstub_session_create(const gdbstub_config *cfg,
                                        const gdbstub_backend *backend,
                                        const gdbstub_transport *transport)
{
  (void)cfg;
  (void)backend;
  (void)transport;
  return (gdbstub_session *)malloc(sizeof(struct gdbstub_session));
}

void gdbstub_session_free(gdbstub_session *s)
{
  free(s);
}

gdbstub_status gdbstub_session_step(gdbstub_session *s,
                                    const gdbstub_event *ev, uint64_t now_ms)
{
  (void)s;
  (void)ev;
  (void)now_ms;
  return GDBSTUB_NIMPL;
}

/* Sentinel state outside the fixture set, so every step's state assertion
 * (READY/TX/WAIT_ACK/RUNNING/DETACH_ACK/CLOSED) fails against the stub. */
gdbstub_state gdbstub_session_state(const gdbstub_session *s)
{
  (void)s;
  return GDBSTUB_STATE_NIMPL;
}

const char *gdbstub_session_reason(const gdbstub_session *s)
{
  (void)s;
  return 0; /* NULL is legal for the caller to test; never non-NULL here */
}

gdbstub_status gdbstub_session_last_status(const gdbstub_session *s)
{
  (void)s;
  return GDBSTUB_NIMPL;
}

gdbstub_status gdbstub_codec_encode(const uint8_t *payload, size_t len,
                                    uint8_t *out, size_t cap, size_t *n)
{
  (void)payload;
  (void)len;
  (void)out;
  (void)cap;
  if (n)
    *n = 0;
  return GDBSTUB_NIMPL;
}

gdbstub_status gdbstub_codec_decode(const uint8_t *frame, size_t len,
                                    uint8_t *out, size_t cap, size_t *n)
{
  (void)frame;
  (void)len;
  (void)out;
  (void)cap;
  if (n)
    *n = 0;
  return GDBSTUB_NIMPL;
}
