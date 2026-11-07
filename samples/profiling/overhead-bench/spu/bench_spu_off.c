/* overhead-bench SPU TU "off": the shared loop source with every writer operation removed (no writer symbol is
 * referenced: the A1 audit checks the object).  PS3DK_PROF=0 here: the SPU image's CFLAGS (-DPS3DK_PROF=1, for the
 * writer sources) are the same for every SPU source, so this TU resets it before any include. */
#undef PS3DK_PROF
#define PS3DK_PROF 0
#include <stdint.h>

#include "../common/ob_timer.h"

#define OB_TU off
#define OB_DEC() ob_dec()
#define OB_SB(name) ((void)sizeof(name), 0u)
#define OB_SE(z) ((void)(z))
#define OB_SM(name) ((void)sizeof(name))
#define OB_FLUSH() 0
#include "bench_spu.inc"
