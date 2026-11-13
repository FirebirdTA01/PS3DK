#include <nv40.h>
#include <rsx.h>
#include <gcm_sys.h>
#include <ppu_intrinsics.h>

#include "rsx_internal.h"
#ifdef PS3TC_PROF_AUTO
#include "../ps3tc_prof_auto.h"
#endif

/* The Logic-OP pair (rsxSetLogicOp / rsxSetLogicOpEnable, and the
 * corresponding *_Unsafe variants) is now a thin alias over the shared
 * native emitters in <cell/gcm/gcm_state_native.h>.  Include <cell/gcm.h>
 * here so those emitters pull in <cell/gcm/gcm_cg_bridge.h> (for the
 * single ps3tc_gcm_reserve + ps3tc_gcm_invoke_callback path shared with
 * the tex/cg bridges) and provide the CellGcmContextData alias. */
#include <cell/gcm.h>

#define RSX_INTERNAL	1

#ifndef RSX_MEMCPY
#define RSX_MEMCPY	__builtin_memcpy
#endif

static __inline__ f32 swapF32_16(f32 v)
{
	ieee32 d;
	d.f = v;
	d.u = ( ( ( d.u >> 16 ) & 0xffff ) << 0 ) | ( ( ( d.u >> 0 ) & 0xffff ) << 16 );
	return d.f;
}

#define RSX_UNSAFE	1
#define RSX_FUNCTION_MACROS
#include <rsx_function_macros.h>
#include "commands_impl.h"
#undef RSX_FUNCTION_MACROS
#include <rsx_function_macros.h>
#undef RSX_UNSAFE

#define RSX_UNSAFE	0
#define RSX_FUNCTION_MACROS
#include <rsx_function_macros.h>
#include "commands_impl.h"
#undef RSX_FUNCTION_MACROS
#include <rsx_function_macros.h>
#undef RSX_UNSAFE

#undef RSX_INTERNAL
