/* libsputhread SPU lv2-syscall wrappers - the event-port family.
 *
 * Four entry points let an SPU thread exchange events with PPU
 * threads through the lv2 sys_event mechanism:
 *
 *   sys_spu_thread_send_event(spup, data0, data1)
 *     Synchronous: writes data1 to the lv2 event port `spup`, with
 *     data0 in the low 24 bits of the event word.  Returns the
 *     kernel's status code (CELL_OK on success).
 *
 *   sys_spu_thread_throw_event(spup, data0, data1)
 *     Fire-and-forget: same wire encoding plus the 0x40000000
 *     "throw" marker in the event word; no kernel reply.  Always
 *     returns CELL_OK on a valid spup, ESRCH on out-of-range.
 *
 *   sys_spu_thread_receive_event(spuq, &d1, &d2, &d3)
 *   sys_spu_thread_tryreceive_event(spuq, &d1, &d2, &d3)
 *     Pull the next event off SPU event queue `spuq`; the kernel
 *     returns the (data1, data2, data3) triple via four ch29 reads
 *     (status + 3 payload words).  receive blocks until one is
 *     available; tryreceive returns 0x8001000a if empty.  Both use
 *     stop 0x110 / 0x111 to drive the kernel.
 *
 * Channel protocol summary:
 *
 *   ch28 (SPU_WrOutMbox)      - syscall payload (event-port: data1
 *                                 mailbox; receive: spuq id)
 *   ch29 (SPU_RdInMbox)       - kernel return value(s)
 *   ch30 (SPU_WrOutIntrMbox)  - tagged event word (port id<<24 |
 *                                 data0[23:0] | optional 0x40000000
 *                                 throw marker | optional 0x80000000
 *                                 set_bit blocking marker)
 *
 * Error codes:
 *   0x80010002  ESRCH       (spup > 63 or invalid)
 *   0x8001000a  EAGAIN      (mailbox full / receive empty)
 *
 * Bytes here closely track the reference libsputhread.a; comments
 * call out the protocol bits.
 */
	.text

/* int sys_spu_thread_send_event(uint8_t spup, uint32_t data0, uint32_t data1)
 *   r3 = spup (port id, 0..63)
 *   r4 = data0 (low 24 bits used)
 *   r5 = data1 (mailbox value)
 * Returns CELL_OK in r3 on success; ESRCH on bad spup; EAGAIN if the
 * mailbox already has a pending response. */
	.globl	sys_spu_thread_send_event
	.type	sys_spu_thread_send_event, @function
	.align	3
sys_spu_thread_send_event:
	clgtbi	$7, $3, 63          /* spup > 63 ? */
	ilhu	$6, 0x8001
	xsbh	$2, $7
	shlqbyi	$7, $4, 0           /* $7 = data0 */
	ori	$6, $6, 2           /* $6 = 0x80010002 (ESRCH) */
	brhz	$2, 1f              /* spup valid: enter syscall body */
.Lse_ret:
	ori	$3, $6, 0
	bi	$0
1:	shli	$8, $3, 24          /* $8 = spup << 24 */
	fsmbi	$9, 0x7777          /* per-slot mask 0x00FFFFFF */
	ilhu	$10, 0x8001
	rchcnt	$4, $ch29           /* depth of inbound mailbox */
	hbrr	2f, .Lse_ret        /* hint: trailing branch returns via .Lse_ret */
	ori	$6, $10, 10         /* $6 = 0x8001000a (EAGAIN) for mailbox-busy */
	and	$3, $7, $9          /* $3 = data0 & 0x00FFFFFF */
	lnop
	or	$2, $8, $3          /* $2 = (spup<<24) | (data0 & 0x00FFFFFF) */
	brnz	$4, .Lse_ret        /* mailbox already pending: bail with EAGAIN */
	wrch	$ch28, $5           /* arg1 = data1 */
	wrch	$ch30, $2           /* arg2 = event word; fires kernel handler */
	rdch	$6, $ch29           /* result code <- ch29 */
2:	br	.Lse_ret
	.size	sys_spu_thread_send_event, . - sys_spu_thread_send_event


/* int sys_spu_thread_throw_event(uint8_t spup, uint32_t data0, uint32_t data1)
 *   r3 = spup, r4 = data0, r5 = data1
 * Fire-and-forget: kernel never replies.  Returns CELL_OK on valid
 * spup, ESRCH otherwise. */
	.globl	sys_spu_thread_throw_event
	.type	sys_spu_thread_throw_event, @function
	.align	3
sys_spu_thread_throw_event:
	clgtbi	$8, $3, 63          /* spup > 63 ? */
	hbr	2f, $0              /* hint: trailing bi $0 returns via $LR */
	ilhu	$6, 0x8001
	xsbh	$2, $8
	shli	$7, $3, 24          /* $7 = spup << 24 */
	ori	$6, $6, 2           /* $6 = 0x80010002 (ESRCH) */
	.long	0x4020007f          /* nop $127 - byte-match the reference;
	                             * SPU GAS folds `nop $127` to `nop $0` */
	brhnz	$2, 1f              /* invalid spup: skip body, return ESRCH */
	ori	$10, $5, 0          /* $10 = data1 */
	fsmbi	$11, 0x7777
	il	$6, 0               /* CELL_OK */
	wrch	$ch28, $10          /* arg1 = data1 */
	and	$9, $4, $11         /* $9 = data0 & 0x00FFFFFF */
	ilhu	$4, 0x4000          /* THROW marker (0x40000000) */
	or	$5, $9, $4          /* $5 = (data0 & 0x00FFFFFF) | 0x40000000 */
	or	$3, $7, $5          /* $3 = (spup<<24) | (data0 & FFFFFF) | THROW */
	wrch	$ch30, $3           /* arg2 = throw event word */
1:	ori	$3, $6, 0
2:	bi	$0
	.size	sys_spu_thread_throw_event, . - sys_spu_thread_throw_event


/* Internal: the receive-side body shared by receive / tryreceive.
 * The two entry points differ only in their `stop` opcode: 0x110 for
 * blocking receive, 0x111 for the non-blocking variant.  Macro-expand
 * to keep the body inline so each function is one self-contained
 * symbol with no external branches. */
.macro RECV_EVENT stop_op
	ilhu	$7, 0x8001
	rchcnt	$2, $ch29           /* depth of inbound mailbox */
	ori	$8, $4, 0           /* $8 = &d1 */
	shlqbyi	$9, $5, 0           /* $9 = &d2 */
	ori	$7, $7, 10          /* $7 = 0x8001000a (EAGAIN) */
	brz	$2, 1f              /* mailbox empty: proceed */
.Lrcv_busy_\@:
	ori	$3, $7, 0           /* mailbox already pending: return EAGAIN */
	bi	$0
1:	ori	$4, $3, 0           /* $4 = spuq */
	hbrr	2f, .Lrcv_busy_\@   /* hint */
	wrch	$ch28, $4           /* arg1 = spuq */
	stop	\stop_op            /* trap: kernel populates ch29 */
	rdch	$7, $ch29           /* status code */
	cwd	$10, 0($8)          /* compute write-mask for *d1 (any byte offset) */
	cwd	$11, 0($5)          /* for *d2 (caller's $5 still holds the ptr) */
	cwd	$12, 0($6)          /* for *d3 */
	brnz	$7, .Lrcv_busy_\@   /* non-zero status: bail */
	lqd	$16, 0($8)          /* load aligned 16 bytes containing *d1 */
	rdch	$17, $ch29          /* d1 word from kernel */
	rdch	$14, $ch29          /* d2 word */
	rdch	$5, $ch29           /* d3 word */
	shufb	$15, $17, $16, $10  /* insert d1 into the right slot */
	stqd	$15, 0($8)          /* write back */
	lqd	$13, 0($9)          /* same for d2 */
	shufb	$10, $14, $13, $11
	stqd	$10, 0($9)
	lqd	$8, 0($6)           /* same for d3 */
	shufb	$3, $5, $8, $12
	stqd	$3, 0($6)
2:	br	.Lrcv_busy_\@       /* return $7 (= 0 on success) */
.endm


/* int sys_spu_thread_receive_event(uint32_t spuq,
 *                                  uint32_t *d1, uint32_t *d2, uint32_t *d3)
 *   r3 = spuq, r4 = &d1, r5 = &d2, r6 = &d3
 * Blocks in the kernel until the next event lands on the queue.  On
 * return, the d1/d2/d3 outputs hold the event payload words. */
	.globl	sys_spu_thread_receive_event
	.type	sys_spu_thread_receive_event, @function
	.align	3
sys_spu_thread_receive_event:
	RECV_EVENT 0x110
	.size	sys_spu_thread_receive_event, . - sys_spu_thread_receive_event


/* int sys_spu_thread_tryreceive_event(uint32_t spuq,
 *                                     uint32_t *d1, uint32_t *d2, uint32_t *d3)
 * Non-blocking sibling: returns 0x8001000a (EAGAIN) immediately if no
 * event is waiting; otherwise identical to receive_event. */
	.globl	sys_spu_thread_tryreceive_event
	.type	sys_spu_thread_tryreceive_event, @function
	.align	3
sys_spu_thread_tryreceive_event:
	RECV_EVENT 0x111
	.size	sys_spu_thread_tryreceive_event, . - sys_spu_thread_tryreceive_event
