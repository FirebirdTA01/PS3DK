/* libspurs_task internal: 128-byte line operations and the running task's
 * context, shared by the SPU-side object implementations. */
#ifndef SPURS_LINE_H
#define SPURS_LINE_H

#include <stdint.h>
#include <spu_mfcio.h>

/* CELL_SPURS_TASK_ERROR_* */
#define TASK_AGAIN    0x80410901u
#define TASK_INVAL    0x80410902u
#define TASK_NOSYS    0x80410903u
#define TASK_NOEXEC   0x80410907u
#define TASK_PERM     0x80410909u
#define TASK_BUSY     0x8041090Au
#define TASK_STAT     0x8041090Fu
#define TASK_ALIGN    0x80410910u
#define TASK_NULL     0x80410911u
#define TASK_FATAL    0x80410914u
#define TASK_SHUTDOWN 0x80410920u

/* the kernel's DMA tag for the running workload (LS 0x1cc) */
static inline unsigned spurs_kernel_tag(void)
{
	return *(volatile uint32_t *)(uintptr_t)0x1cc;
}

/* LS 0x2fd8: the object a blocked task waits on, tagged with its kind */
#define TASK_WAIT_OBJECT 0x2fd8

static inline void line_get_at(void *line, uint64_t ea)
{
	mfc_getllar(line, ea, 0, 0);
	(void)mfc_read_atomic_status();
	spu_dsync();
}

static inline int line_put_at(void *line, uint64_t ea)
{
	spu_dsync();
	mfc_putllc(line, ea, 0, 0);
	return (mfc_read_atomic_status() & MFC_PUTLLC_STATUS) == 0;
}

static inline void dma_wait(unsigned tag)
{
	mfc_write_tag_mask(1u << tag);
	(void)mfc_read_tag_status_all();
}

static inline void dma_get_wait(volatile void *ls, uint64_t ea, uint32_t size, unsigned tag)
{
	mfc_get(ls, ea, size, tag, 0, 0);
	dma_wait(tag);
}

static inline void dma_put_wait(volatile void *ls, uint64_t ea, uint32_t size, unsigned tag)
{
	mfc_put(ls, ea, size, tag, 0, 0);
	dma_wait(tag);
}

/* big-endian field access inside an LS copy of a line */
#define LINE_U8(l, off)  (((volatile uint8_t *)(l))[(off)])
#define LINE_U32(l, off) (*(volatile uint32_t *)((volatile uint8_t *)(l) + (off)))
#define LINE_U64(l, off) (*(volatile uint64_t *)((volatile uint8_t *)(l) + (off)))

#endif
