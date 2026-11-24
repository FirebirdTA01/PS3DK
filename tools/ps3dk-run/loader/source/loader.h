/* ps3dk-loader: shared declarations. */
#ifndef PS3DK_LOADER_H
#define PS3DK_LOADER_H

#include <pthread.h>
#include <stdint.h>

/* status lines shown on screen (and printed to the TTY) */
void loader_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t loader_next_run_id(void);
void loader_clean_runs(void);
/* one authenticated session on an accepted connection; returns when the session ends (a run that spawned does not
 * return) */
void loader_session(int fd, const char *peer_ip);
/* called right before exitspawn: stops rendering so the GPU is idle when the process is replaced */
void loader_before_spawn(void);
void loader_after_failed_spawn(void);
/* shutdown lifetime (main.c): every network wait in the worker is a slice of at most LOADER_SLICE_MS that rechecks
   loader_stopping(); loader_begin_spawn() refuses once a stop was requested */
#define LOADER_SLICE_MS 250
int loader_stopping(void);
int loader_begin_spawn(void);

/* ---- key and state (state.c); state/ is never reachable through the file service ---- */
int loader_key(uint8_t key[32]);              /* 0 and the key when paired, -1 when not */
const char *loader_fingerprint(void);         /* first 16 hex digits of SHA-256(key), or "(not paired)" */
int loader_import_key(void);                  /* TRIANGLE: /dev_usb00[01]/PS3DKRUN.KEY -> state/key; 0 on success */
void loader_revoke_key(void);                 /* SQUARE: delete state/key */
void loader_state_init(int returning);        /* cold start: files OFF; return from a run: restore files-on */
int loader_random(void *p, unsigned n);       /* sys_get_random_number; 0 on success */

/* ---- file service switch (files.c) ---- */
extern pthread_mutex_t files_lock;            /* serialises the switch against PUT-publish and DELETE */
extern volatile int files_on;
extern volatile unsigned files_gen;           /* incremented on every OFF: cancels outstanding file work */
void loader_set_files(int on, const char *who);
/* file requests on an authenticated session (files.c): returns 0 to continue the session, -1 to end it */
struct prn_chan_s;
int loader_file_request(void *chan, uint8_t type, uint8_t *payload, uint32_t n);

#endif
