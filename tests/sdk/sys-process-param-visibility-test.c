/*
 * sys-process-param-visibility-test.c — Verify SYS_PROCESS_PARAM visibility
 *
 * Verifies that <cell/spurs.h> and <sys/dbg.h> expose SYS_PROCESS_PARAM
 * without explicit <sys/process.h> inclusion, matching reference SDK behavior.
 */

#if defined(TEST_INCLUDE_CELL_SPURS)
# include <cell/spurs.h>
#elif defined(TEST_INCLUDE_SYS_DBG)
# include <sys/dbg.h>
#else
# error "Specify TEST_INCLUDE_CELL_SPURS or TEST_INCLUDE_SYS_DBG"
#endif

/* Both reference samples atrac3plus_spurs and face_track_filter use: */
SYS_PROCESS_PARAM(1001, 0x10000)

int main(void)
{
    return 0;
}
