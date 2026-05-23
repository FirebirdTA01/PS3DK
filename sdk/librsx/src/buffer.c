#include <stdlib.h>
#include <unistd.h>
#include <rsx/gcm_sys.h>
#include <rsx/commands.h>
#include <ppu_intrinsics.h>

#define RSX_INTERNAL	0

#define RSX_UNSAFE	1
#define RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#include "buffer_impl.h"
#undef RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#undef RSX_UNSAFE

#define RSX_UNSAFE	0
#define RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#include "buffer_impl.h"
#undef RSX_FUNCTION_MACROS
#include <rsx/rsx_function_macros.h>
#undef RSX_UNSAFE
