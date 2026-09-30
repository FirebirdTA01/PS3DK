/* cell/daisy/lfqueue2_types.h - the 256-byte, 128-byte-aligned area a
 * QueueControl::Atomic stream shares between its ends (zero it before the
 * first constructor). */
#ifndef PS3TC_CELL_DAISY_LFQUEUE2_TYPES_H
#define PS3TC_CELL_DAISY_LFQUEUE2_TYPES_H

namespace cell {
namespace Daisy {

typedef struct LFQueue2 {
	unsigned char skip[256];
} __attribute__((aligned(128))) LFQueue2;

} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_LFQUEUE2_TYPES_H */
