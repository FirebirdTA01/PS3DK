/* Private D5 runtime acceptance seam. NOT a public SDK option or stable ABI.
 * Declarations exist only in host test builds. Runtime admission additionally
 * requires PS3DK_PROF_ROLLING; ordinary archives/defaults remain disabled. */
#ifndef PS3DK_PROF_ROLL_RUNTIME_TEST_H
#define PS3DK_PROF_ROLL_RUNTIME_TEST_H
#ifndef PS3DK_PROF_TEST_HOOKS
#error "private rolling runtime seam requires PS3DK_PROF_TEST_HOOKS"
#endif
#include "roll_runtime_types.h"
typedef p_roll_options ps3dk_prof_test_rolling_options;
typedef p_roll_record ps3dk_prof_test_rolling_record;
enum { PS3DK_PROF_TEST_EROLLRESOURCE = P_ROLL_RESOURCE };
/* Pre-init, same serialization/error contract as ordinary setters. Disabled
 * leaves append unchanged; its zero capacities have no default meaning.
 * Enabled requires explicit capacities and a FILE sink. Never mask flags to
 * fit the PPU/WINDOWS encoder. Unsupported families/ARC fail before creation. */
int ps3dk_prof_test_rolling_set(const ps3dk_prof_test_rolling_options *);
/* Separate private SPU selector. options.ppu must be entirely zero; combined
 * is the sole semantic capacity source. No manual GPU/AUTO/FIFO admission. */
int ps3dk_prof_test_rolling_combined_set(const ps3dk_prof_test_rolling_options *, const pfm_config *);
/* Errors leave out untouched: ENOTINIT before successful init, ENOTSUP for
 * append, EBUSY until first PUBLIC shutdown publishes after close/join.
 * Automatic stopping does not consume that public claim. Success is immutable.
 * Physical state, committed frontier and separate errors do not imply durability. */
int ps3dk_prof_test_rolling_result(ps3dk_prof_test_rolling_record *out);
#endif
