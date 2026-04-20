#include <nv40.h>
#include <rsx.h>
#include <gcm_sys.h>
#include <ppu_intrinsics.h>

#include "rsx_internal.h"

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
