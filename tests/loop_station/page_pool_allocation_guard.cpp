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
	alignas(Frame) std::byte storage[4 * kPageBytes]{};
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
	// Detaching must retain the sealed record and republishing must not copy pages.
	for (unsigned cycle = 0; cycle < 3; ++cycle) {
		const auto active = pool.sealedHandle(0);
		if (!active.valid())
			return 6;
		const auto expectedL = pool.frame(active, 0)->l;
		const auto expectedR = pool.frame(active, 0)->r;
		PhraseHandle retained{};
		if (pool.detach(0, retained) != Status::Ok)
			return 7;
		if (pool.sealedHandle(0).valid() || retained.generation != active.generation)
			return 8;
		if (pool.length(retained) != 2 || pool.frame(retained, 0)->l != expectedL
		    || pool.frame(retained, 0)->r != expectedR)
			return 9;
		PhraseHandle candidate{};
		if (pool.beginDetached(0, 2, candidate) != Status::Ok)
			return 10;
		if (pool.reserveDetachedPages(candidate, PagePool::kMaxPagesPerRecord) != Status::Ok)
			return 11;
		if (pool.writeDetached(candidate, 0, {int32_t(cycle), -int32_t(cycle)}) != Status::Ok
		    || pool.writeDetached(candidate, 1, {int32_t(cycle + 1), -int32_t(cycle + 1)}) != Status::Ok)
			return 12;
		if (pool.sealDetached(candidate) != Status::Ok)
			return 13;
		PhraseHandle previous{};
		if (pool.replace(candidate, previous) != Status::Ok || previous.valid())
			return 14;
		if (pool.length(pool.sealedHandle(0)) != 2 || pool.frame(pool.sealedHandle(0), 1)->l != int32_t(cycle + 1))
			return 15;
		for (unsigned i = 0; i < 1000; ++i)
			if (pool.frame(pool.sealedHandle(0), 0)->l != int32_t(cycle))
				return 16;
		if (pool.length(retained) != 2 || pool.frame(retained, 0)->l != expectedL)
			return 17;
	}
	return allocationCalls == before ? 0 : 5;
}
