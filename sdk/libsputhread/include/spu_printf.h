/* spu_printf.h - SPU-side stdio compatibility header.
 *
 * Reference samples include this at the top level (`<spu_printf.h>`);
 * PSL1GHT keeps the same declaration under `<sys/spu_printf.h>`.
 * Forward to the existing PSL1GHT header so callers get the same
 * `_spu_call_event_va_arg` + `spu_printf(fmt, args...)` macro
 * regardless of the spelling.
 */
#ifndef __PS3DK_SPU_PRINTF_H__
#define __PS3DK_SPU_PRINTF_H__

#include <sys/spu_printf.h>

#endif /* __PS3DK_SPU_PRINTF_H__ */
