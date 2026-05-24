/*
 * heap.inl — Inline doubly-linked free-list operations for the block
 * allocator.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#ifndef HEAP_INL_H
#define HEAP_INL_H

#include <ppu-asm.h>

#ifndef _HAssert
#define _HAssert(x) ((void)0)
#endif

static __inline__ heap_block *
__heap_head(heap_cntrl *theheap)
{
	return &theheap->free_list;
}

static __inline__ heap_block *
__heap_tail(heap_cntrl *theheap)
{
	/* Head and tail point at the same in-struct sentinel — the free list
	 * is a circular doubly-linked list, so traversal terminates when
	 * ->next wraps back to &free_list.  Using a tail outside the struct
	 * (e.g. heap_end - HEADER_SIZE) leaves last_block->next / ->prev
	 * uninitialized and triggers wild writes when __heap_block_replace
	 * touches them during the first allocation. */
	return &theheap->free_list;
}

static __inline__ heap_block *
__heap_first(heap_cntrl *theheap)
{
	return __heap_head(theheap)->next;
}

static __inline__ heap_block *
__heap_last(heap_cntrl *theheap)
{
	return __heap_tail(theheap)->prev;
}

static __inline__ uintptr_t
__heap_block_size(const heap_block *block)
{
	return block->size & ~HEAP_BLOCK_PREV_USED;
}

static __inline__ bool
__heap_prev_used(const heap_block *block)
{
	return (block->size & HEAP_BLOCK_PREV_USED) != 0;
}

static __inline__ heap_block *
__heap_block_at(const heap_block *block, uintptr_t offset)
{
	return (heap_block *)((uintptr_t)block + offset);
}

static __inline__ heap_block *
__heap_block_prev(const heap_block *block)
{
	return __heap_block_at(block, -(block->prev_size));
}

static __inline__ uintptr_t
__heap_alloc_area_of_block(const heap_block *block)
{
	return (uintptr_t)block + HEAP_BLOCK_HEADER_SIZE;
}

static __inline__ void
__heap_block_set_size(heap_block *block, uintptr_t size)
{
	block->size = size | (block->size & HEAP_BLOCK_PREV_USED);
}

static __inline__ void
__heap_block_remove(heap_block *block)
{
	block->prev->next = block->next;
	block->next->prev = block->prev;
}

static __inline__ void
__heap_block_insert_after(heap_block *anchor, heap_block *block)
{
	block->next = anchor->next;
	block->prev = anchor;
	anchor->next->prev = block;
	anchor->next = block;
}

static __inline__ void
__heap_block_replace(heap_block *old_block, heap_block *new_block)
{
	new_block->next = old_block->next;
	new_block->prev = old_block->prev;
	old_block->next->prev = new_block;
	old_block->prev->next = new_block;
}

static __inline__ heap_block *
__heap_block_of_alloc_area(uintptr_t alloc_begin, uintptr_t page_size)
{
	return (heap_block *)(alloc_begin - HEAP_BLOCK_HEADER_SIZE);
}

static __inline__ uintptr_t
__heap_align_up(uintptr_t value, uintptr_t alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static __inline__ uintptr_t
__heap_align_down(uintptr_t value, uintptr_t alignment)
{
	return value & ~(alignment - 1);
}

static __inline__ uintptr_t
__heap_min_block_size(uintptr_t page_size)
{
	return HEAP_BLOCK_HEADER_SIZE + page_size;
}

static __inline__ bool
__heap_is_aligned(uintptr_t value, uintptr_t alignment)
{
	return (value & (alignment - 1)) == 0;
}

static __inline__ void
__heap_set_last_block_size(heap_cntrl *theheap)
{
	/* last_block carries a wrap-around size: walking forward by this
	 * value lands on first_block.  __heap_prev_used(next_next_block)
	 * during a split therefore tests first_block's PREV_USED bit (set
	 * in heapInit) rather than last_block's (which is never marked
	 * used) — keeps the split path on __heap_block_insert_after and
	 * away from __heap_block_replace, which would otherwise dereference
	 * the uninitialized last_block->next / ->prev. */
	__heap_block_set_size(theheap->last_block,
		(uintptr_t)theheap->first_block - (uintptr_t)theheap->last_block);
}

#endif
