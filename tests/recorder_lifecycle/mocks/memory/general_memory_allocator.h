#pragma once
#include <cstddef>
class GeneralMemoryAllocator {
public:
	static GeneralMemoryAllocator& get();
	void* allocStealable(size_t size);
};
void delugeDealloc(void* memory);

void* delugeAlloc(unsigned int size, bool mayUseOnChipRam = true);
