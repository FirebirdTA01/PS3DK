/* cell/daisy/daisy_defs.h - libdaisy common definitions.
 *
 * libdaisy builds data streams between producers and consumers (SPU threads,
 * SPURS tasks, PPU threads): a stream is a Buffer (where entries live) and a
 * QueueControl (who may use which entry when), wrapped by a Pipe port: the
 * producer's InPort pushes, the consumer's OutPort pops.  Delivery is in
 * order and complete. */
#ifndef PS3TC_CELL_DAISY_DEFS_H
#define PS3TC_CELL_DAISY_DEFS_H

#include <stdint.h>
#include <string.h>
#include <cell/error.h>
#include <cell/daisy/return_code.h>
#include <cell/daisy/error.h>

#ifdef __SPU__
#include <spu_intrinsics.h>
#endif

namespace cell {
namespace Daisy {

/* number of entries in a queue */
typedef uint32_t SizeType;
/* a queue entry number */
typedef uint32_t PointerType;

/* how the parameter-exchange constructors are used (SPU) */
enum ConstructorMode {
	NO_PARAMETER = 0,   /* the caller gives every argument */
	PARAMETER    = 1    /* the two ends exchange them through shared memory */
};

/* which end of the queue a queue control drives */
enum QueueIO {
	OUTPUT,             /* consumer */
	INPUT               /* producer */
};

/* how push and pop reach an entry */
enum BufferMode {
	COPY      = 0,      /* data is copied in and out */
	REFERENCE = 1       /* the entry itself is handed out (local buffers) */
};

/* which callback a Glue step calls */
enum GlueMode {
	TWO_PORT = 2,       /* callback(out, in) */
	ONE_PORT = 1        /* callback(inout) */
};

enum BufferType {
	BUFFER_TYPE_LOCAL,
	BUFFER_TYPE_REMOTE,
	BUFFER_TYPE_STRIDE_REMOTE
};

enum QueueControlType {
	QCTL_TYPE_LOCAL,
	QCTL_TYPE_SIGNAL_NOTIFICATION,
	QCTL_TYPE_ATOMIC,
	QCTL_TYPE_SHARED_MEMORY,
	QCTL_TYPE_PRIVATE_MEMORY
};

} /* namespace Daisy */
} /* namespace cell */

#endif /* PS3TC_CELL_DAISY_DEFS_H */
