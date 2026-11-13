#ifndef PS3DK_PROF_AUTO_COMMANDS_H
#define PS3DK_PROF_AUTO_COMMANDS_H
#include <stdint.h>
#include "ps3tc_prof_auto.h"
#define auto_command_words ps3dk_prof__auto_command_words
#define auto_command_room ps3dk_prof__auto_command_room
/* Zero excludes observation; it never repairs or suppresses the application
 * command. Matches the pinned safe emitters' complete reservation groups. */
uint64_t auto_command_words(const ps3tc_auto_call *);
/* Integer validation precedes pointer arithmetic. end names the reserved tail
 * JUMP word: the complete application command plus both six-word markers must
 * fit before it, without a callback or an additional flush. */
int auto_command_room(uintptr_t begin, uintptr_t current, uintptr_t end, uint64_t words);
#endif
