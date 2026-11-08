/* I3a link fixture TU3: one zone in a function nothing references (kept only by the __start_/__stop_ rule). */
#include "prof_spu_lane.h"

void tu3_dead(ps3dk_prof_spu_writer *w);
void tu3_dead(ps3dk_prof_spu_writer *w) { PS3DK_PROF_SPU_MARK(w, "tu3_dead"); }
