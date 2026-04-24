/* PS3 Custom Toolchain - top-level types.h compat forwarder.
 *
 * Reference SDK samples include <types.h> as a sibling of <sys/types.h>;
 * we forward through so unmodified Sony sources compile against our
 * include tree.
 */
#ifndef __PS3DK_TYPES_H__
#define __PS3DK_TYPES_H__

#include <sys/types.h>
#include <stdint.h>

#endif /* __PS3DK_TYPES_H__ */
