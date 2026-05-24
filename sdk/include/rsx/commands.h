/*! \file commands.h
 \brief RSX commands.

These are functions to enqueue commands into the RSX's command buffer.
*/

#ifndef __RSX_COMMANDS_H__
#define __RSX_COMMANDS_H__

#include <rsx/gcm_sys.h>
#include <rsx/rsx_program.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RSX_INTERNAL	0

#define RSX_UNSAFE	1
#define RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#include <rsx/commands_inc.h>
#undef RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#undef RSX_UNSAFE

#define RSX_UNSAFE	0
#define RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#include <rsx/commands_inc.h>
#undef RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#undef RSX_UNSAFE

#undef RSX_INTERNAL

#ifdef __cplusplus
	}
#endif

#endif
