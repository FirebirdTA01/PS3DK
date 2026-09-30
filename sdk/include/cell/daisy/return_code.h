/* cell/daisy/return_code.h - non-error results of libdaisy calls. */
#ifndef PS3TC_CELL_DAISY_RETURN_CODE_H
#define PS3TC_CELL_DAISY_RETURN_CODE_H

#include <cell/error.h>

namespace cell {
namespace Daisy {

enum ReturnCode {
	QUEUE_IS_BUSY = 2,   /* a try-call could not start or end now */
	TERMINATED    = 3    /* every producer has terminated and the queue is drained */
};

} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_RETURN_CODE_H */
