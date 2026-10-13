/*
 * PS3 Custom Toolchain - <cell/hash/libhash.h> error codes shared by the
 * message-digest libraries (libmd5, libsha1, libsha224, libsha256,
 * libsha384, libsha512).
 */

#ifndef PS3TC_CELL_HASH_LIBHASH_H
#define PS3TC_CELL_HASH_LIBHASH_H

#define CELL_ERROR_HASH_NULL_POINTER            0x80f00001
#define CELL_ERROR_HASH_QUEUE_CREATE_FAILED     0x80f00002
#define CELL_ERROR_HASH_FLAG_CREATE_FAILED      0x80f00003
#define CELL_ERROR_HASH_INVALID_ALIGN           0x80f00004
#define CELL_ERROR_HASH_INVALID_NOMEM           0x80f00005
#define CELL_ERROR_HASH_TASK_CREATE_FAILED      0x80f00006
#define CELL_ERROR_HASH_INVALID_STATE           0x80f00007
#define CELL_ERROR_HASH_INVALID_BUFFER_SIZE     0x80f00008
#define CELL_ERROR_HASH_INVALID_STREAM_ID       0x80f00009

#endif /* PS3TC_CELL_HASH_LIBHASH_H */
