/* key_objects.c -- New and Delete for the six keyed object types.
 *
 * New: take a reference on the key.  If the key was empty this caller is
 * the creator: it allocates the object's block, initialises it (with the
 * heap's DMA tag) and publishes the address; on any failure it frees the
 * block and cancels, so the key is empty again.  Otherwise the published
 * object is reused as it is: the other arguments are ignored and the type
 * is not checked.
 *
 * Delete: drop the reference; whoever drops the last one frees the block
 * and empties the key.  The caller's record is left unchanged.
 */
#include <cell/atomic.h>
#include <cell/sync.h>
#include <cell/sheap.h>
#include "sheap_spu.h"

/* Header snapshot for key operations, separate from the heap functions'
 * lock-time copy because the creator calls those in between. */
static sheap_header key_header;

/* Validate, snapshot the header and take a reference on `key`.  *object is
 * 0 when this caller must create the object. */
static int attach(const void *obj, uint64_t ea_ksheap, CellSheapKey key,
                  uint64_t *object)
{
    if (obj == 0 || ea_ksheap == 0 || key >= CELL_SHEAP_NUM_KEY_ENTRY)
        return (int)CELL_SHEAP_ERROR_INVAL;
    if (ea_ksheap & (SHEAP_HEADER_BYTES - 1))
        return (int)CELL_SHEAP_ERROR_ALIGN;
    __sheap_fetch(ea_ksheap, &key_header);
    if (key_header.ea_keytable == 0)
        return (int)CELL_SHEAP_ERROR_INVAL;     /* not an initialised keyed heap */
    *object = __sheap_key_new_object(&key_header, key);
    return CELL_OK;
}

/* Creator: allocate the block, or cancel creation. */
static int create(uint64_t ea_ksheap, CellSheapKey key, uint64_t bytes,
                  uint64_t *object)
{
    *object = cellSheapAllocate(ea_ksheap, bytes);
    if (*object != 0)
        return CELL_OK;
    (void)__sheap_key_finalize_new(&key_header, key, 0);
    return (int)CELL_SHEAP_ERROR_SHORTAGE;
}

/* Creator: publish an initialised object, or undo creation when its
 * initialisation failed with `init_rc`. */
static int publish(uint64_t ea_ksheap, CellSheapKey key, uint64_t object,
                   int init_rc)
{
    if (init_rc != CELL_OK) {
        (void)cellSheapFree(ea_ksheap, object);
        (void)__sheap_key_finalize_new(&key_header, key, 0);
        return init_rc;
    }
    return __sheap_key_finalize_new(&key_header, key, object);
}

static void detach(uint64_t ea_ksheap, CellSheapKey key)
{
    uint64_t object = 0;

    if (ea_ksheap == 0 || (ea_ksheap & (SHEAP_HEADER_BYTES - 1))
        || key >= CELL_SHEAP_NUM_KEY_ENTRY)
        return;
    __sheap_fetch(ea_ksheap, &key_header);
    if (key_header.ea_keytable == 0)
        return;
    if (__sheap_key_delete_object(&key_header, key, &object) != CELL_OK
        || object == 0)
        return;
    (void)cellSheapFree(ea_ksheap, object);
    (void)__sheap_key_finalize_delete(&key_header, key);
}

#define FILL_RECORD(rec, heap, k, block) \
    do {                                 \
        (rec)->ea_ksheap = (heap);       \
        (rec)->key = (k);                \
        (rec)->ea = (block);             \
    } while (0)

/* ---- buffer --------------------------------------------------------- */

int cellKeySheapBufferNew(CellKeySheapBuffer *obj, uint64_t ea_ksheap,
                          CellSheapKey key, uint64_t size)
{
    uint64_t object;
    int rc = attach(obj, ea_ksheap, key, &object);

    if (rc == CELL_OK && object == 0) {
        rc = create(ea_ksheap, key, size, &object);
        if (rc == CELL_OK)
            rc = publish(ea_ksheap, key, object, CELL_OK);
    }
    if (rc != CELL_OK)
        return rc;
    FILL_RECORD(obj, ea_ksheap, key, object);
    obj->size = size;
    return CELL_OK;
}

void cellKeySheapBufferDelete(CellKeySheapBuffer *obj)
{
    if (obj)
        detach(obj->ea_ksheap, obj->key);
}

/* ---- mutex ---------------------------------------------------------- */

int cellKeySheapMutexNew(CellKeySheapMutex *obj, uint64_t ea_ksheap,
                         CellSheapKey key)
{
    uint64_t object;
    int rc = attach(obj, ea_ksheap, key, &object);

    if (rc == CELL_OK && object == 0) {
        rc = create(ea_ksheap, key, 128, &object);
        if (rc == CELL_OK) {
            /* A fresh 128-byte block always satisfies the mutex's rules. */
            (void)cellSyncMutexInitialize(object, key_header.spu_tag1);
            rc = publish(ea_ksheap, key, object, CELL_OK);
        }
    }
    if (rc != CELL_OK)
        return rc;
    FILL_RECORD(obj, ea_ksheap, key, object);
    return CELL_OK;
}

void cellKeySheapMutexDelete(CellKeySheapMutex *obj)
{
    if (obj)
        detach(obj->ea_ksheap, obj->key);
}

/* ---- barrier -------------------------------------------------------- */

int cellKeySheapBarrierNew(CellKeySheapBarrier *obj, uint64_t ea_ksheap,
                           CellSheapKey key, uint16_t count)
{
    uint64_t object;
    int rc = attach(obj, ea_ksheap, key, &object);

    if (rc == CELL_OK && object == 0) {
        rc = create(ea_ksheap, key, 128, &object);
        if (rc == CELL_OK)
            rc = publish(ea_ksheap, key, object,
                         cellSyncBarrierInitialize(object, count,
                                                   key_header.spu_tag1));
    }
    if (rc != CELL_OK)
        return rc;
    FILL_RECORD(obj, ea_ksheap, key, object);
    return CELL_OK;
}

void cellKeySheapBarrierDelete(CellKeySheapBarrier *obj)
{
    if (obj)
        detach(obj->ea_ksheap, obj->key);
}

/* ---- queue ---------------------------------------------------------- */

int cellKeySheapQueueNew(CellKeySheapQueue *obj, uint64_t ea_ksheap,
                         CellSheapKey key, uint32_t buffer_size,
                         uint32_t depth)
{
    uint64_t object;
    int rc = attach(obj, ea_ksheap, key, &object);

    if (rc == CELL_OK && object == 0) {
        /* Queue header line, then the ring (element bytes wrap at 32 bits
         * like the firmware's). */
        uint64_t bytes = (uint64_t)(uint32_t)(buffer_size * depth) + 128;

        rc = create(ea_ksheap, key, bytes, &object);
        if (rc == CELL_OK)
            rc = publish(ea_ksheap, key, object,
                         cellSyncQueueInitialize(object, object + 128,
                                                 buffer_size, depth,
                                                 key_header.spu_tag1));
    }
    if (rc != CELL_OK)
        return rc;
    FILL_RECORD(obj, ea_ksheap, key, object);
    return CELL_OK;
}

void cellKeySheapQueueDelete(CellKeySheapQueue *obj)
{
    if (obj)
        detach(obj->ea_ksheap, obj->key);
}

/* ---- reader/writer buffer ------------------------------------------- */

int cellKeySheapRwmNew(CellKeySheapRwm *obj, uint64_t ea_ksheap,
                       CellSheapKey key, uint32_t buffer_size)
{
    uint64_t object;
    int rc = attach(obj, ea_ksheap, key, &object);

    if (rc == CELL_OK && object == 0) {
        rc = create(ea_ksheap, key, (uint64_t)buffer_size + 128, &object);
        if (rc == CELL_OK)
            rc = publish(ea_ksheap, key, object,
                         cellSyncRwmInitialize(object, object + 128,
                                               buffer_size,
                                               key_header.spu_tag1));
    }
    if (rc != CELL_OK)
        return rc;
    FILL_RECORD(obj, ea_ksheap, key, object);
    return CELL_OK;
}

void cellKeySheapRwmDelete(CellKeySheapRwm *obj)
{
    if (obj)
        detach(obj->ea_ksheap, obj->key);
}

/* ---- semaphore ------------------------------------------------------ */

static uint32_t semaphore_line[32] __attribute__((aligned(128)));

int cellKeySheapSemaphoreNew(CellKeySheapSemaphore *obj, uint64_t ea_ksheap,
                             CellSheapKey key, int count)
{
    uint64_t object;
    int rc = attach(obj, ea_ksheap, key, &object);

    if (rc == CELL_OK && object == 0) {
        rc = create(ea_ksheap, key, 128, &object);
        if (rc == CELL_OK) {
            (void)cellAtomicStore32(semaphore_line, object, (uint32_t)count);
            rc = publish(ea_ksheap, key, object, CELL_OK);
        }
    }
    if (rc != CELL_OK)
        return rc;
    FILL_RECORD(obj, ea_ksheap, key, object);
    return CELL_OK;
}

void cellKeySheapSemaphoreDelete(CellKeySheapSemaphore *obj)
{
    if (obj)
        detach(obj->ea_ksheap, obj->key);
}
