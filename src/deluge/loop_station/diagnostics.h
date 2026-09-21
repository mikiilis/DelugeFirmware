#pragma once
#include <cstdint>

namespace deluge::loop_station {
// RAM-only debugger context. Never inspect a suspect String allocation to fill this.
struct OwnershipContext {
	uintptr_t song = 0;
	uintptr_t clip = 0;
	uintptr_t recorder = 0;
	uintptr_t pendingRecorder = 0;
	uint32_t slot = UINT32_MAX;
	uint32_t operation = 0; // 1=start, 2=finish, 3=publish/rollback
};
inline OwnershipContext ownershipContext;
struct OwnershipScope {
	OwnershipContext previous;
	explicit OwnershipScope(OwnershipContext context) : previous(ownershipContext) { ownershipContext = context; }
	~OwnershipScope() { ownershipContext = previous; }
};
struct StringFault {
	OwnershipContext context;
	uintptr_t destination;
	uintptr_t source;
	uintptr_t memory;
	uintptr_t caller;
};
inline volatile StringFault lastStringFault{};
inline void snapshotStringFault(void const* destination, void const* source, void const* memory, void* caller) {
	lastStringFault.context.song = ownershipContext.song;
	lastStringFault.context.clip = ownershipContext.clip;
	lastStringFault.context.recorder = ownershipContext.recorder;
	lastStringFault.context.pendingRecorder = ownershipContext.pendingRecorder;
	lastStringFault.context.slot = ownershipContext.slot;
	lastStringFault.context.operation = ownershipContext.operation;
	lastStringFault.destination = reinterpret_cast<uintptr_t>(destination);
	lastStringFault.source = reinterpret_cast<uintptr_t>(source);
	lastStringFault.memory = reinterpret_cast<uintptr_t>(memory);
	lastStringFault.caller = reinterpret_cast<uintptr_t>(caller);
}
} // namespace deluge::loop_station
