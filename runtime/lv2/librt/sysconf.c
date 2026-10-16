/* sysconf over Lv-2: the processor counts only.
 * SPDX-License-Identifier: BSD-2-Clause */
#include <errno.h>
#include <unistd.h>

/* std::thread::hardware_concurrency asks for _SC_NPROCESSORS_ONLN.  The PPE
 * runs two hardware threads; the SPUs are not processors in this sense (they
 * do not run PPU threads).  Every other name is EINVAL, as POSIX specifies
 * for a name this system does not support. */
long
sysconf(int name)
{
	switch (name) {
	case _SC_NPROCESSORS_CONF:
	case _SC_NPROCESSORS_ONLN:
		return 2;
	default:
		errno = EINVAL;
		return -1;
	}
}
