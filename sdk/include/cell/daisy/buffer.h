/* cell/daisy/buffer.h - the base of the Buffer templates: where a stream's
 * entries (tSize of tType) are kept. */
#ifndef PS3TC_CELL_DAISY_BUFFER_H
#define PS3TC_CELL_DAISY_BUFFER_H

#include <cell/daisy/daisy_defs.h>

namespace cell {
namespace Daisy {
namespace Buffer {

template <typename tType, SizeType tSize>
class Abstract {
public:
	typedef tType DataType;
	static const SizeType sSize = tSize;
};

} /* namespace Buffer */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_BUFFER_H */
