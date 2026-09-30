/* _cellOvisUpdateAndMapSection($3 = &__ovly_info_<section>)
 *
 * Called by the wrappers cellOvisConfigAuto generates in front of each
 * overlaid function: maps the section (gCellOvisTable, gCellOvisTag) and
 * waits for it.  The wrapper keeps the first argument ($3) itself; this
 * keeps $2 and the argument registers $4..$14 across the two C calls, so a
 * wrapped function with up to twelve register arguments sees them intact. */
	.text
	.align	3
	.global	_cellOvisUpdateAndMapSection
	.type	_cellOvisUpdateAndMapSection, @function
_cellOvisUpdateAndMapSection:
	ai	$sp, $sp, -288
	stqd	$2, 32($sp)
	stqd	$4, 64($sp)
	stqd	$5, 80($sp)
	stqd	$6, 96($sp)
	stqd	$7, 112($sp)
	stqd	$8, 128($sp)
	stqd	$9, 144($sp)
	stqd	$10, 160($sp)
	stqd	$11, 176($sp)
	stqd	$12, 192($sp)
	stqd	$13, 208($sp)
	stqd	$14, 224($sp)
	stqd	$80, 240($sp)
	stqd	$lr, 256($sp)
	/* back chain: the caller's frame */
	ai	$2, $sp, 288
	stqd	$2, 0($sp)
	ori	$80, $3, 0		/* &info, kept across the call */
	/* load offset of this code: run-time minus link-time address */
	ila	$2, 1f
	brsl	$6, 1f
1:	sf	$6, $2, $6
	lqd	$3, 0($80)		/* info (by value) */
	lqr	$4, gCellOvisTable	/* 64-bit, preferred slot words 0-1 */
	lqr	$5, gCellOvisTag
	/* both are in 16-byte slots of their own quadword: rotate to preferred */
	ila	$7, gCellOvisTable
	rotqby	$4, $4, $7
	ila	$7, gCellOvisTag
	rotqby	$5, $5, $7
	brsl	$lr, _cellOvisStartMapping
	ori	$3, $80, 0
	lqr	$4, gCellOvisTag
	ila	$7, gCellOvisTag
	rotqby	$4, $4, $7
	brsl	$lr, _cellOvisWaitMapping
	ori	$3, $80, 0
	lqd	$lr, 256($sp)
	lqd	$80, 240($sp)
	lqd	$2, 32($sp)
	lqd	$4, 64($sp)
	lqd	$5, 80($sp)
	lqd	$6, 96($sp)
	lqd	$7, 112($sp)
	lqd	$8, 128($sp)
	lqd	$9, 144($sp)
	lqd	$10, 160($sp)
	lqd	$11, 176($sp)
	lqd	$12, 192($sp)
	lqd	$13, 208($sp)
	lqd	$14, 224($sp)
	ai	$sp, $sp, 288
	bi	$lr
	.size	_cellOvisUpdateAndMapSection, .-_cellOvisUpdateAndMapSection
