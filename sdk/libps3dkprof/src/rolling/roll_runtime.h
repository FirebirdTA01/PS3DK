/* Private host-only runtime prototype. No public option or default admission. */
#ifndef PS3DK_PROF_ROLL_RUNTIME_H
#define PS3DK_PROF_ROLL_RUNTIME_H
#if defined(PS3DK_PROF_ROLLING) && defined(PS3DK_PROF_TEST_HOOKS)
#define P_ROLLING_RUNTIME 1
#include "roll_runtime_test.h"
#include "roll_file.h"
#define p_roll_plan ps3dk_prof__roll_runtime_plan
#define p_roll_bind ps3dk_prof__roll_runtime_bind
#define p_roll_start ps3dk_prof__roll_runtime_start
#define p_roll_accept ps3dk_prof__roll_runtime_accept
#define p_roll_flush ps3dk_prof__roll_runtime_flush
#define p_roll_finish ps3dk_prof__roll_runtime_finish
#define p_roll_publish ps3dk_prof__roll_runtime_publish
struct prof;
typedef struct p_roll_runtime {
    ps3dk_prof_test_rolling_options options;
    pfm_config combined;
    uint32_t combined_enabled;
    ps3dk_prof_test_rolling_record record;
    pfr_layout disk;
    pfp_layout encoder;
    pfr_file file;
    pfr_io io;
    pfr_store *store;
    pfo_output *output;
    pfo_buffers buffers;
    void *initial, *scratch;
    uint32_t stop_owner, stop_prepared; /* atomic; distinct from public shutdown claim */
    int result;
} p_roll_runtime;
int p_roll_plan(const ps3dk_prof_test_rolling_options *, const pfm_config *, size_t *);
void p_roll_bind(struct prof *, void *, const ps3dk_prof_test_rolling_options *, const pfm_config *, size_t);
int p_roll_start(struct prof *, const uint8_t[16]);
int p_roll_accept(struct prof *, const void *, size_t);
int p_roll_flush(struct prof *);
int p_roll_finish(struct prof *);
void p_roll_publish(struct prof *, int);
#endif
#endif
