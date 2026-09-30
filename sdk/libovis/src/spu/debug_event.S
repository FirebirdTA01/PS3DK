/* _ovly_debug_event - does nothing; a debugger sets a breakpoint here and
 * re-reads _ovly_table to learn which overlay sections are resident. */
	.text
	.align	3
	.global	_ovly_debug_event
	.type	_ovly_debug_event, @function
_ovly_debug_event:
	bi	$lr
	.size	_ovly_debug_event, .-_ovly_debug_event
