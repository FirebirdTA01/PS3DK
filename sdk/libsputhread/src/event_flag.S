/* libsputhread SPU lv2-syscall wrappers — sys_event_flag_set_bit + impatient.
 *
 * SPU↔kernel lv2 syscall protocol via SPU mailbox channels:
 *
 *   ch28 (SPU_WrOutMbox)      - syscall arg 1 (here: sys_event_flag_t ef)
 *   ch29 (SPU_RdInMbox)       - syscall return slot
 *   ch30 (SPU_WrOutIntrMbox)  - magic-tagged syscall id; writing this
 *                                wakes the kernel.  High half 0x8001
 *                                marks "lv2 syscall"; low half holds
 *                                the opcode.  An additional 0x80000000
 *                                bit on top of the bitn arg requests
 *                                the BLOCKING flavour (waits for a
 *                                free FIFO slot if the receiver is
 *                                full); without it the IMPATIENT path
 *                                runs which can drop and return EBUSY.
 *
 * sys_event_flag_set_bit waits for ch29 to drain (rchcnt $ch29 != 0)
 * before issuing the syscall, so it'll spin against EBUSY internally
 * and only return CELL_OK or a parameter-validation error.
 */
	.text

/* int sys_event_flag_set_bit(sys_event_flag_t ef, uint8_t bitn)
 *   r3 = ef (sys_event_flag_t)
 *   r4 = bitn
 * Returns CELL_OK in r3 on success, parameter error otherwise. */
	.globl	sys_event_flag_set_bit
	.type	sys_event_flag_set_bit, @function
	.align	3
sys_event_flag_set_bit:
	clgtbi	$6, $4, 63           /* bit > 63 ? */
	ilhu	$5, 0x8001
	xsbh	$2, $6
	ori	$5, $5, 2            /* $5 = 0x80010002 = SYS_EVENT_FLAG_SET_BIT */
	brhz	$2, 1f               /* fall through to spinning loop if valid */
	ori	$3, $5, 0            /* invalid: return the syscall id-marker as error */
	nop	$127
	bi	$0
1:	ori	$6, $3, 0            /* save ef in $6 */
	rchcnt	$7, $ch29            /* result-mailbox depth */
	andi	$3, $4, 0xff
	hbrr	2f, 1b
	ilhu	$8, 0x8001
	ilhu	$4, 0x8000           /* BLOCKING request bit */
	ori	$5, $8, 10
	lnop
	or	$3, $3, $4           /* arg = bitn | 0x80000000 */
	brnz	$7, 1b               /* drain pending result first */
	wrch	$ch28, $6
	wrch	$ch30, $3            /* fire syscall */
2:	rdch	$5, $ch29            /* read result */
	br	1b                   /* loop on EBUSY */
	.size	sys_event_flag_set_bit, . - sys_event_flag_set_bit


/* int sys_event_flag_set_bit_impatient(sys_event_flag_t ef, uint8_t bitn)
 *   r3 = ef
 *   r4 = bitn
 * Non-blocking: returns immediately with CELL_OK on success, or an
 * error code if the receiver FIFO is full / parameters are invalid. */
	.globl	sys_event_flag_set_bit_impatient
	.type	sys_event_flag_set_bit_impatient, @function
	.align	3
sys_event_flag_set_bit_impatient:
	clgtbi	$7, $4, 63           /* bit > 63 ? */
	hbr	1f, $0
	ilhu	$2, 0xc000
	andi	$4, $4, 0xff
	ilhu	$6, 0x8001
	xsbh	$5, $7
	or	$4, $4, $2           /* arg = bitn | 0xc0000000 (impatient marker) */
	shlqbyi	$2, $3, 0
	ori	$6, $6, 2            /* syscall id 0x80010002 */
	brhnz	$5, 2f               /* invalid bit: skip syscall */
	il	$6, 0                /* preset CELL_OK return */
	wrch	$ch28, $2
	wrch	$ch30, $4            /* fire syscall, no result wait */
2:	ori	$3, $6, 0
1:	bi	$0
	.size	sys_event_flag_set_bit_impatient, . - sys_event_flag_set_bit_impatient
