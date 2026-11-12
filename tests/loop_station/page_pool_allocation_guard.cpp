#include "deluge/loop_station/page_pool.h"

#include <cstdlib>
#include <new>

static unsigned allocationCalls = 0;

void* operator new(std::size_t size) {
	++allocationCalls;
	if (void* p = std::malloc(size))
		return p;
	throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
	++allocationCalls;
	if (void* p = std::malloc(size))
		return p;
	throw std::bad_alloc();
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
	++allocationCalls;
	return std::malloc(size);
}
void operator delete(void* p) noexcept {
	std::free(p);
}
void operator delete[](void* p) noexcept {
	std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
	std::free(p);
}

int main() {
	using namespace loop_station;
	alignas(Frame) std::byte storage[2 * kPageBytes]{};
	PagePool pool;
	if (pool.initialize(storage) != Status::Ok)
		return 1;
	const unsigned before = allocationCalls;
	if (pool.beginRenderRound(0x01) != Status::Ok || pool.beginPhrase(0, 0) != Status::Ok)
		return 1;
	const Frame input[2] = {{42, -42}, {7, -7}};
	FrameIndex accepted = 0;
	if (pool.append(0, input, accepted) != Status::Ok || accepted != 2)
		return 2;
	PhraseHandle handle;
	if (pool.seal(0, handle) != Status::Ok || pool.frame(handle, 1)->r != -7)
		return 3;
	for (unsigned i = 0; i < 1000; ++i)
		if (pool.frame(handle, 0)->l != 42)
			return 4;
	return allocationCalls == before ? 0 : 5;
}
