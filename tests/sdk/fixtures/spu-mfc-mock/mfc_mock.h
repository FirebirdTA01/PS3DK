/* Simulated main memory and MFC for host tests of SPU library code.
 *
 * Effective addresses are offsets into mfc_mock_memory.  Every command is
 * checked (size, alignment, bounds, LS/EA low-bit match) and fails the
 * test loudly when illegal.  Line reservations are modelled: a getllar
 * reserves the line, and a putllc succeeds only if no other store touched
 * the line since; mfc_mock_foreign_store stands for a store by another
 * processor (the PPU) and breaks the reservation.
 *
 * Byte order: the SPU is big-endian, the host usually is not.  With
 * mfc_mock_swap32 set, every transfer swaps each 4-byte group, so code
 * that only accesses 32-bit words in LS (the sync mutex and barrier) sees
 * the same values it would on the SPU while the simulated memory holds
 * true big-endian bytes.  Leave it clear for code that mixes widths; its
 * memory is then in host order.
 *
 * mfc_mock_on_reserve, when set, runs before every getllar: tests use it
 * to act as the other processor while SPU code spins.
 *
 * Reservation loss: mfc_mock_on_putllc, when set, runs when a putllc
 * arrives, before the reservation is checked.  It can store to the line
 * as another processor (mfc_mock_foreign_store), which loses the
 * reservation as real hardware would, or return nonzero to lose it with
 * no change (e.g. a store to an unrelated part of the line).  A lost
 * putllc writes nothing.  mfc_mock_putllc_attempts / _lost count them.
 */
#ifndef MFC_MOCK_H
#define MFC_MOCK_H

#include <stdint.h>

#define MFC_MOCK_MEMORY (8u << 20)

extern uint8_t mfc_mock_memory[MFC_MOCK_MEMORY];
extern int mfc_mock_swap32;
extern void (*mfc_mock_on_reserve)(uint64_t line_ea);
extern unsigned long mfc_mock_reserves;
extern int (*mfc_mock_on_putllc)(uint64_t line_ea);
extern unsigned long mfc_mock_putllc_attempts;
extern unsigned long mfc_mock_putllc_lost;

/* A ready-made on_putllc hook: loses every other attempt, no change. */
int mfc_mock_lose_alternate(uint64_t line_ea);

void mfc_mock_foreign_store(uint64_t ea, const void *bytes, uint32_t size);

#endif /* MFC_MOCK_H */
