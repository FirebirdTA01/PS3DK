/* cell/daisy/qctl.h - the base of the QueueControl templates: which entry of
 * a tSize ring each end may use when.  tQueueIO is INPUT for the producer's
 * control, OUTPUT for the consumer's.
 *
 * Every queue control offers the same operations to the Pipe ports:
 *   int  tryReserve(PointerType *entry)  CELL_OK, QUEUE_IS_BUSY, or
 *                                        TERMINATED (OUTPUT: drained and
 *                                        every producer has terminated)
 *   bool isTurn(PointerType entry)       may this reservation complete now
 *   void complete(PointerType entry)     publish (INPUT) / free (OUTPUT) it
 *   int  terminate()                     detach this end
 *   bool hasUnfinishedConsumer()         INPUT: consumers still attached */
#ifndef PS3TC_CELL_DAISY_QCTL_H
#define PS3TC_CELL_DAISY_QCTL_H

#include <cell/daisy/daisy_defs.h>

namespace cell {
namespace Daisy {
namespace QueueControl {

template <SizeType tSize, QueueIO tQueueIO>
class Abstract {
public:
	static const SizeType sSize = tSize;
	static const QueueIO sPort = tQueueIO;
};

} /* namespace QueueControl */
} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_QCTL_H */
