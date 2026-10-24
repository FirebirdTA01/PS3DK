#pragma once

#include <cstddef>

class MemoryPool
{
public:
	MemoryPool() = default;
	~MemoryPool() = default;
	void* allocate(size_t size);
	void deallocate(void* ptr);
};