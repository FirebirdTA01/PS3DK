/*
 * heap.c — Block-based heap allocator for Lv-2 sbrk backing store.
 *
 * Replaces the libsysbase heap allocator previously provided by the
 * upstream prefix.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 PS3 Custom Toolchain Contributors
 */

#include <stdlib.h>
#include <string.h>
#include <ppu-asm.h>
#include <lv2/spinlock.h>

#include "heap.h"
#include "heap.inl"

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

static bool
__heap_get_first_last_block(uintptr_t heap_begin, uintptr_t heap_size,
                            uintptr_t page_size, uintptr_t min_block_size,
                            heap_block **first_block, heap_block **last_block)
{
	uintptr_t heap_end = heap_begin + heap_size;
	uintptr_t alloc_begin = __heap_align_up(heap_begin + HEAP_BLOCK_HEADER_SIZE, page_size);
	uintptr_t first_block_begin = alloc_begin - HEAP_BLOCK_HEADER_SIZE;
	uintptr_t overhead = HEAP_BLOCK_HEADER_SIZE + (first_block_begin - heap_begin);
	uintptr_t first_block_size = __heap_align_down(heap_size - overhead, page_size);
	heap_block *start_block = (heap_block *)first_block_begin;
	heap_block *end_block = __heap_block_at(start_block, first_block_size);

	if (heap_end < heap_begin || heap_size <= overhead ||
	    first_block_size < min_block_size)
		return false;

	*first_block = start_block;
	*last_block = end_block;
	return true;
}

static void
__heap_block_split(heap_cntrl *theheap, heap_block *block,
                   heap_block *next_block, heap_block *free_anchor,
                   uintptr_t alloc_size)
{
	uintptr_t page_size       = theheap->page_size;
	uintptr_t min_block_size  = theheap->min_block_size;
	uintptr_t min_alloc_size  = min_block_size - HEAP_BLOCK_HEADER_SIZE;
	uintptr_t block_size      = __heap_block_size(block);
	uintptr_t used_size       = MAX(alloc_size, min_alloc_size) + HEAP_BLOCK_HEADER_SIZE;
	uintptr_t used_block_size = __heap_align_up(used_size, page_size);
	uintptr_t free_size       = block_size + HEAP_BLOCK_ALLOC_BONUS - used_size;
	uintptr_t free_size_limit = min_block_size + HEAP_BLOCK_ALLOC_BONUS;

	_HAssert(used_size <= block_size + HEAP_BLOCK_ALLOC_BONUS);
	_HAssert(used_size + free_size == block_size + HEAP_BLOCK_ALLOC_BONUS);

	if (free_size >= free_size_limit) {
		heap_block *free_block = __heap_block_at(block, used_block_size);
		uintptr_t free_block_size = block_size - used_block_size;
		uintptr_t next_block_size = __heap_block_size(next_block);
		heap_block *next_next_block = __heap_block_at(next_block, next_block_size);

		_HAssert(used_block_size + free_block_size == block_size);
		__heap_block_set_size(block, used_block_size);

		if (__heap_prev_used(next_next_block))
			__heap_block_insert_after(free_anchor, free_block);
		else {
			__heap_block_replace(next_block, free_block);
			free_block_size += next_block_size;
			next_block = __heap_block_at(free_block, free_block_size);
		}

		free_block->size = free_block_size | HEAP_BLOCK_PREV_USED;
		next_block->prev_size = free_block_size;
		next_block->size &= ~HEAP_BLOCK_PREV_USED;
	} else {
		next_block->size |= HEAP_BLOCK_PREV_USED;
	}
}

static heap_block *
__heap_block_allocate_from_begin(heap_cntrl *theheap, heap_block *block,
                                 heap_block *next_block, heap_block *free_anchor,
                                 uintptr_t alloc_size)
{
	__heap_block_split(theheap, block, next_block, free_anchor, alloc_size);
	return block;
}

static heap_block *
__heap_block_allocate_from_end(heap_cntrl *theheap, heap_block *block,
                               heap_block *next_block, heap_block *free_anchor,
                               uintptr_t alloc_begin, uintptr_t alloc_size)
{
	heap_block *new_block = __heap_block_of_alloc_area(alloc_begin, theheap->page_size);
	uintptr_t new_block_begin = (uintptr_t)new_block;
	uintptr_t new_block_size = (uintptr_t)next_block - new_block_begin;
	uintptr_t block_size_adjusted = (uintptr_t)new_block - (uintptr_t)block;

	_HAssert(block_size_adjusted >= theheap->min_block_size);
	_HAssert(new_block_size >= theheap->min_block_size);

	if (__heap_prev_used(block)) {
		__heap_block_insert_after(free_anchor, block);
		free_anchor = block;
	} else {
		heap_block *prev_block = __heap_block_prev(block);
		uintptr_t prev_block_size = __heap_block_size(prev_block);
		block = prev_block;
		block_size_adjusted += prev_block_size;
	}

	block->size = block_size_adjusted | HEAP_BLOCK_PREV_USED;
	new_block->prev_size = block_size_adjusted;
	new_block->size = new_block_size;

	__heap_block_split(theheap, new_block, next_block, free_anchor, alloc_size);
	return new_block;
}

static heap_block *
__heap_block_allocate(heap_cntrl *theheap, heap_block *block,
                      uintptr_t alloc_begin, uintptr_t alloc_size)
{
	uintptr_t alloc_area_begin = __heap_alloc_area_of_block(block);
	uintptr_t alloc_area_offset = alloc_begin - alloc_area_begin;
	uintptr_t block_size = __heap_block_size(block);
	heap_block *next_block = __heap_block_at(block, block_size);
	heap_block *free_anchor = NULL;

	_HAssert(alloc_area_begin <= alloc_begin);

	if (__heap_prev_used(next_block))
		free_anchor = __heap_head(theheap);
	else {
		free_anchor = block->prev;
		__heap_block_remove(block);
	}

	if (alloc_area_offset < theheap->page_size) {
		alloc_size += alloc_area_offset;
		block = __heap_block_allocate_from_begin(theheap, block, next_block,
							free_anchor, alloc_size);
	} else {
		block = __heap_block_allocate_from_end(theheap, block, next_block,
						       free_anchor, alloc_begin, alloc_size);
	}
	return block;
}

static uintptr_t
__heap_check_block(const heap_cntrl *theheap, const heap_block *block,
                   uintptr_t alloc_size, uintptr_t alignment)
{
	uintptr_t page_size      = theheap->page_size;
	uintptr_t min_block_size = theheap->min_block_size;
	uintptr_t block_begin    = (uintptr_t)block;
	uintptr_t block_size     = __heap_block_size(block);
	uintptr_t block_end      = block_begin + block_size;
	uintptr_t alloc_begin_floor = __heap_alloc_area_of_block(block);
	uintptr_t alloc_begin_ceiling = block_end - min_block_size +
					HEAP_BLOCK_HEADER_SIZE + page_size - 1;
	uintptr_t alloc_end      = block_end + HEAP_BLOCK_ALLOC_BONUS;
	uintptr_t alloc_begin    = alloc_end - alloc_size;

	alloc_begin = __heap_align_down(alloc_begin, alignment);
	if (alloc_begin > alloc_begin_ceiling)
		alloc_begin = __heap_align_down(alloc_begin_ceiling, alignment);
	alloc_end = alloc_begin + alloc_size;

	if (alloc_begin >= alloc_begin_floor) {
		uintptr_t alloc_block_begin =
			(uintptr_t)__heap_block_of_alloc_area(alloc_begin, page_size);
		uintptr_t free_size = alloc_block_begin - block_begin;
		if (free_size >= min_block_size || free_size == 0)
			return alloc_begin;
	}
	return 0;
}

uintptr_t
heapInit(heap_cntrl *theheap, void *start_addr, uintptr_t size)
{
	uintptr_t heap_begin = (uintptr_t)start_addr;
	uintptr_t heap_end = heap_begin + size;
	uintptr_t min_block_size = __heap_min_block_size(CPU_ALIGNMENT);
	heap_block *first_block = NULL;
	heap_block *last_block = NULL;

	if (!__heap_get_first_last_block(heap_begin, size, CPU_ALIGNMENT,
					  min_block_size, &first_block, &last_block))
		return 0;

	memset(theheap, 0, sizeof(heap_cntrl));

	uintptr_t first_block_begin = (uintptr_t)first_block;
	uintptr_t last_block_begin  = (uintptr_t)last_block;
	uintptr_t first_block_size  = last_block_begin - first_block_begin;

	first_block->prev_size = heap_end;
	first_block->size = first_block_size | HEAP_BLOCK_PREV_USED;
	first_block->next = __heap_tail(theheap);
	first_block->prev = __heap_head(theheap);

	theheap->page_size      = CPU_ALIGNMENT;
	theheap->min_block_size = min_block_size;
	theheap->heap_begin     = heap_begin;
	theheap->heap_end       = heap_end;
	theheap->first_block    = first_block;
	theheap->last_block     = last_block;
	__heap_head(theheap)->next = first_block;
	__heap_tail(theheap)->prev = first_block;

	last_block->prev_size = first_block_size;
	last_block->size = 0;
	__heap_set_last_block_size(theheap);

	sysSpinlockInitialize(&theheap->lock);

	_HAssert(__heap_is_aligned(theheap->page_size, CPU_ALIGNMENT));
	_HAssert(__heap_is_aligned(theheap->min_block_size, CPU_ALIGNMENT));
	_HAssert(__heap_is_aligned(__heap_alloc_area_of_block(first_block), CPU_ALIGNMENT));
	_HAssert(__heap_is_aligned(__heap_alloc_area_of_block(last_block), CPU_ALIGNMENT));

	return first_block_size;
}

void *
heapAllocateAligned(heap_cntrl *theheap, uintptr_t size, uintptr_t alignment)
{
	uintptr_t block_size_floor = size + HEAP_BLOCK_HEADER_SIZE - HEAP_BLOCK_ALLOC_BONUS;
	heap_block *tail = __heap_tail(theheap);
	heap_block *block = NULL;
	uintptr_t alloc_begin = 0;

	if (block_size_floor < size)
		return NULL;

	sysSpinlockLock(&theheap->lock);

	block = __heap_first(theheap);
	while (block != tail) {
		_HAssert(__heap_prev_used(block));
		if (block->size > block_size_floor) {
			if (alignment == 0)
				alloc_begin = __heap_alloc_area_of_block(block);
			else
				alloc_begin = __heap_check_block(theheap, block, size, alignment);
		}
		if (alloc_begin != 0)
			break;
		block = block->next;
	}

	if (alloc_begin != 0)
		block = __heap_block_allocate(theheap, block, alloc_begin, size);

	sysSpinlockUnlock(&theheap->lock);
	return (void *)alloc_begin;
}

void *
heapAllocate(heap_cntrl *theheap, uintptr_t size)
{
	return heapAllocateAligned(theheap, size, 0);
}

bool
heapFree(heap_cntrl *theheap, void *ptr)
{
	if (!ptr)
		return true;

	sysSpinlockLock(&theheap->lock);

	heap_block *block = __heap_block_of_alloc_area((uintptr_t)ptr,
							theheap->page_size);
	uintptr_t prev_size = block->prev_size;
	uintptr_t block_size = __heap_block_size(block);
	heap_block *next_block = __heap_block_at(block, block_size);

	if (__heap_prev_used(next_block))
		__heap_block_insert_after(__heap_head(theheap), block);
	else
		__heap_block_replace(next_block, block);

	if (!__heap_prev_used(block)) {
		heap_block *prev_block = __heap_block_prev(block);
		prev_size += __heap_block_size(prev_block);
		block = prev_block;
	}

	block->size &= HEAP_BLOCK_PREV_USED;
	block->size += prev_size;

	if (!__heap_prev_used(next_block))
		block->size += __heap_block_size(next_block);

	next_block = __heap_block_at(block, __heap_block_size(block));
	next_block->prev_size = __heap_block_size(block);
	next_block->size &= ~HEAP_BLOCK_PREV_USED;

	sysSpinlockUnlock(&theheap->lock);
	return true;
}
