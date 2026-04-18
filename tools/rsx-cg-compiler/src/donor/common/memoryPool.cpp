#include "memoryPool.h"
#include <cstdlib>

void* MemoryPool::allocate(size_t size)
{
	return malloc(size);
}

void MemoryPool::deallocate(void* ptr)
{
	free(ptr);
}