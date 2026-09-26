/* Host stand-in for spu_mfcio.h: SPU library sources compile unchanged on
 * the host and their DMA becomes copies into a simulated main memory
 * (see mfc_mock.h).  Only the commands the SDK's SPU libraries use exist.
 */
#ifndef MFC_MOCK_SPU_MFCIO_H
#define MFC_MOCK_SPU_MFCIO_H

#include <stdint.h>

#define MFC_PUTLLC_STATUS  0x00000001u
#define MFC_PUT_CMD        0x20u
#define MFC_GET_CMD        0x40u
#define MFC_PUTLLUC_CMD    0xb0u
#define MFC_PUTLLC_CMD     0xb4u
#define MFC_GETLLAR_CMD    0xd0u

void mfc_mock_command(volatile void *ls, uint64_t ea, uint32_t size,
                      uint32_t tag, uint32_t cmd);
uint32_t mfc_mock_atomic_status(void);

#define mfc_put(ls, ea, size, tag, tid, rid) \
    mfc_mock_command((ls), (ea), (size), (tag), MFC_PUT_CMD)
#define mfc_get(ls, ea, size, tag, tid, rid) \
    mfc_mock_command((ls), (ea), (size), (tag), MFC_GET_CMD)
#define mfc_getllar(ls, ea, tid, rid) \
    mfc_mock_command((ls), (ea), 128, 0, MFC_GETLLAR_CMD)
#define mfc_putllc(ls, ea, tid, rid) \
    mfc_mock_command((ls), (ea), 128, 0, MFC_PUTLLC_CMD)
#define mfc_putlluc(ls, ea, tid, rid) \
    mfc_mock_command((ls), (ea), 128, 0, MFC_PUTLLUC_CMD)
#define mfc_read_atomic_status() mfc_mock_atomic_status()
#define mfc_write_tag_mask(mask) ((void)(mask))
#define mfc_read_tag_status_all() mfc_mock_atomic_status()

#endif /* MFC_MOCK_SPU_MFCIO_H */
