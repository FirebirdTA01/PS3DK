/* cell/sheap/sheap_types.h -- shared-heap key and keyed-object types.
 *
 * Shared by the PPU and SPU halves of libsheap: a keyed object record is
 * filled in by whichever side called the New function and only its `ea`
 * refers to shared state, but both sides must agree on the record so a
 * program can pass one between them.  Every record is 8-byte aligned with
 * the key in its own 8-byte slot; the checks below pin the layout.
 */
#ifndef __PS3DK_CELL_SHEAP_SHEAP_TYPES_H__
#define __PS3DK_CELL_SHEAP_SHEAP_TYPES_H__

#include <stddef.h>
#include <stdint.h>

/* Keys index the 256-entry table at the head of a keyed heap. */
typedef uint32_t CellSheapKey;

#define CELL_SHEAP_NUM_KEY_ENTRY 256

typedef struct CellKeySheapBuffer {
    uint64_t     ea_ksheap;   /* keyed heap the object lives in */
    CellSheapKey key;
    uint64_t     ea;          /* the object's block */
    uint64_t     size;        /* the size this caller passed to New */
} CellKeySheapBuffer;

typedef struct CellKeySheapMutex {
    uint64_t     ea_ksheap;
    CellSheapKey key;
    uint64_t     ea;
} CellKeySheapMutex;

typedef struct CellKeySheapBarrier {
    uint64_t     ea_ksheap;
    CellSheapKey key;
    uint64_t     ea;
} CellKeySheapBarrier;

typedef struct CellKeySheapQueue {
    uint64_t     ea_ksheap;
    CellSheapKey key;
    uint64_t     ea;
} CellKeySheapQueue;

typedef struct CellKeySheapRwm {
    uint64_t     ea_ksheap;
    CellSheapKey key;
    uint64_t     ea;
} CellKeySheapRwm;

typedef struct CellKeySheapSemaphore {
    uint64_t     ea_ksheap;
    CellSheapKey key;
    uint64_t     ea;
} CellKeySheapSemaphore;

/* Layout checks.  A negative array size fails every C and C++ dialect;
 * each check gets its own typedef name so no dialect sees a redefinition. */
#define __PS3DK_SHEAP_CHECK(name, cond) \
    typedef char __ps3dk_sheap_layout_##name[(cond) ? 1 : -1] __attribute__((unused))

#define __PS3DK_SHEAP_CHECK_OBJECT(type, bytes)                            \
    __PS3DK_SHEAP_CHECK(type##_ea_ksheap, offsetof(type, ea_ksheap) == 0); \
    __PS3DK_SHEAP_CHECK(type##_key, offsetof(type, key) == 8);             \
    __PS3DK_SHEAP_CHECK(type##_ea, offsetof(type, ea) == 16);              \
    __PS3DK_SHEAP_CHECK(type##_align, __alignof__(type) == 8);             \
    __PS3DK_SHEAP_CHECK(type##_size, sizeof(type) == (bytes))

__PS3DK_SHEAP_CHECK_OBJECT(CellKeySheapBuffer, 32);
__PS3DK_SHEAP_CHECK(CellKeySheapBuffer_size_field, offsetof(CellKeySheapBuffer, size) == 24);
__PS3DK_SHEAP_CHECK_OBJECT(CellKeySheapMutex, 24);
__PS3DK_SHEAP_CHECK_OBJECT(CellKeySheapBarrier, 24);
__PS3DK_SHEAP_CHECK_OBJECT(CellKeySheapQueue, 24);
__PS3DK_SHEAP_CHECK_OBJECT(CellKeySheapRwm, 24);
__PS3DK_SHEAP_CHECK_OBJECT(CellKeySheapSemaphore, 24);

#undef __PS3DK_SHEAP_CHECK_OBJECT
#undef __PS3DK_SHEAP_CHECK

#endif /* __PS3DK_CELL_SHEAP_SHEAP_TYPES_H__ */
