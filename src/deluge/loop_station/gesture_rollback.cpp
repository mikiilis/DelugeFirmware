#include "gesture_rollback.h"

#include "invariant.h"
#include <algorithm>
#include <memory>

namespace loop_station {

Status GestureRollback::initialize(std::span<std::byte> storage) {
	if (initialized_)
		return Status::Busy;
	if (storage.size() < requiredBytes())
		return Status::NoMemory;
	if (reinterpret_cast<uintptr_t>(storage.data()) % alignof(Entry))
		return Status::Invalid;
	static_assert((kCapacity * sizeof(Entry) + (kCapacity + 1) * sizeof(Descriptor)) % alignof(Entry) == 0);
	for (auto& slot : slots_) {
		slot.entries = reinterpret_cast<Entry*>(storage.data());
		storage = storage.subspan(kCapacity * sizeof(Entry));
		slot.descriptors = reinterpret_cast<Descriptor*>(storage.data());
		storage = storage.subspan((kCapacity + 1) * sizeof(Descriptor));
		for (Index i = 0; i < kCapacity; ++i) {
			std::construct_at(&slot.entries[i]);
			slot.entries[i].next = i + 1 < kCapacity ? i + 1 : nil;
		}
		for (Index i = 0; i <= kCapacity; ++i) {
			std::construct_at(&slot.descriptors[i]);
			slot.descriptors[i].next = i < kCapacity ? i + 1 : nil;
		}
	}
	initialized_ = true;
	return Status::Ok;
}

Status GestureRollback::bind(SlotId id, PhraseHandle handle) {
	if (!initialized_ || !validSlot(id) || handle.slot != id)
		return Status::Invalid;
	const auto length = pool_.length(handle);
	if (!length)
		return Status::Stale;
	auto& slot = slots_[id];
	if (slot.handle.generation == handle.generation && slot.handle.slot == handle.slot)
		return Status::Ok;
	if (slot.root != nil || slot.active != nil)
		return Status::Busy;
	++slot.revision;
	slot.handle = handle;
	slot.length = length;
	return Status::Ok;
}

Status GestureRollback::begin(SlotId id, SampleClock now, FrameIndex phase, FrameIndex phraseFrames) {
	if (!initialized_ || !validSlot(id) || !phraseFrames || phase >= phraseFrames)
		return Status::Invalid;
	auto& slot = slots_[id];
	if (!pool_.length(slot.handle))
		return Status::Stale;
	if (phraseFrames != slot.length)
		return Status::Invalid;
	if (slot.active != nil)
		return Status::Busy;
	if (now < slot.clock)
		return Status::Invalid;
	// At most one descriptor per occupied entry, plus this empty active one.
	detail::invariant(slot.freeDescriptor != nil);
	slot.active = slot.freeDescriptor;
	auto& descriptor = slot.descriptors[slot.active];
	slot.freeDescriptor = descriptor.next;
	descriptor = {};
	slot.start = slot.clock = now;
	return Status::Ok;
}

int GestureRollback::height(const Slot& slot, Index entry) {
	return entry == nil ? 0 : slot.entries[entry].height;
}
void GestureRollback::updateHeight(Slot& slot, Index entry) {
	auto& node = slot.entries[entry];
	node.height = 1 + std::max(height(slot, node.left), height(slot, node.right));
}
GestureRollback::Index GestureRollback::find(const Slot& slot, FrameIndex key) const {
	auto current = slot.root;
	while (current != nil) {
		++work_.treeSteps;
		const auto& node = slot.entries[current];
		if (node.key == key)
			return current;
		current = key < node.key ? node.left : node.right;
	}
	return nil;
}
GestureRollback::Index GestureRollback::rotateLeft(Slot& slot, Index root) {
	++work_.treeSteps;
	const auto pivot = slot.entries[root].right;
	slot.entries[root].right = slot.entries[pivot].left;
	slot.entries[pivot].left = root;
	updateHeight(slot, root);
	updateHeight(slot, pivot);
	return pivot;
}
GestureRollback::Index GestureRollback::rotateRight(Slot& slot, Index root) {
	++work_.treeSteps;
	const auto pivot = slot.entries[root].left;
	slot.entries[root].left = slot.entries[pivot].right;
	slot.entries[pivot].right = root;
	updateHeight(slot, root);
	updateHeight(slot, pivot);
	return pivot;
}
GestureRollback::Index GestureRollback::balance(Slot& slot, Index root) {
	if (root == nil)
		return root;
	++work_.treeSteps;
	updateHeight(slot, root);
	const auto& node = slot.entries[root];
	const int difference = height(slot, node.left) - height(slot, node.right);
	if (difference > 1) {
		const auto& left = slot.entries[node.left];
		if (height(slot, left.left) < height(slot, left.right))
			slot.entries[root].left = rotateLeft(slot, node.left);
		return rotateRight(slot, root);
	}
	if (difference < -1) {
		const auto& right = slot.entries[node.right];
		if (height(slot, right.right) < height(slot, right.left))
			slot.entries[root].right = rotateRight(slot, node.right);
		return rotateLeft(slot, root);
	}
	return root;
}
GestureRollback::Index GestureRollback::insert(Slot& slot, Index root, Index entry) {
	if (root == nil)
		return entry;
	++work_.treeSteps;
	if (slot.entries[entry].key < slot.entries[root].key)
		slot.entries[root].left = insert(slot, slot.entries[root].left, entry);
	else
		slot.entries[root].right = insert(slot, slot.entries[root].right, entry);
	return balance(slot, root);
}
GestureRollback::Index GestureRollback::erase(Slot& slot, Index root, FrameIndex key) {
	detail::invariant(root != nil);
	++work_.treeSteps;
	auto& node = slot.entries[root];
	if (key < node.key)
		node.left = erase(slot, node.left, key);
	else if (key > node.key)
		node.right = erase(slot, node.right, key);
	else {
		if (node.left == nil)
			return node.right;
		if (node.right == nil)
			return node.left;
		// Relink the successor itself, keeping every other entry's FIFO identity.
		Index successor = nil;
		node.right = extractMinimum(slot, node.right, successor);
		slot.entries[successor].left = node.left;
		slot.entries[successor].right = node.right;
		root = successor;
	}
	return balance(slot, root);
}

GestureRollback::Index GestureRollback::extractMinimum(Slot& slot, Index root, Index& minimum) {
	++work_.treeSteps;
	auto& node = slot.entries[root];
	if (node.left == nil) {
		minimum = root;
		return node.right;
	}
	node.left = extractMinimum(slot, node.left, minimum);
	return balance(slot, root);
}

void GestureRollback::append(Slot& slot, Index entry, Index& head, Index& tail) {
	auto& node = slot.entries[entry];
	node.previous = tail;
	node.next = nil;
	if (tail == nil)
		head = entry;
	else
		slot.entries[tail].next = entry;
	tail = entry;
}
void GestureRollback::unlink(Slot& slot, Index entry, Index& head, Index& tail) {
	const auto& node = slot.entries[entry];
	if (node.previous == nil)
		head = node.next;
	else
		slot.entries[node.previous].next = node.next;
	if (node.next == nil)
		tail = node.previous;
	else
		slot.entries[node.next].previous = node.previous;
}
void GestureRollback::releaseOwner(Slot& slot, Index owner) {
	auto& descriptor = slot.descriptors[owner];
	detail::invariant(descriptor.count);
	if (--descriptor.count == 0) {
		descriptor.next = slot.freeDescriptor;
		slot.freeDescriptor = owner;
	}
}
void GestureRollback::removeRetired(Slot& slot, Index entry, bool restore) {
	const auto& node = slot.entries[entry];
	if (restore && slot.descriptors[node.owner].resolution == Resolution::Cancelled) {
		if (auto* frame = pool_.frame(slot.handle, node.key)) {
			*frame = node.original;
			++work_.restoredFrames;
		}
	}
	unlink(slot, entry, slot.retiredHead, slot.retiredTail);
	slot.root = erase(slot, slot.root, node.key);
	releaseOwner(slot, node.owner);
	slot.entries[entry].next = slot.freeEntry;
	slot.freeEntry = entry;
	++work_.retiredFrames;
}
void GestureRollback::retireOne(Slot& slot) {
	if (slot.retiredHead != nil)
		removeRetired(slot, slot.retiredHead, true);
}

void GestureRollback::beforeWrite(SlotId id, FrameIndex index, Frame original) {
	if (!initialized_ || !validSlot(id))
		return;
	auto& slot = slots_[id];
	if (index >= slot.length)
		return;
	// Retire before allocating. Even adversarial cancel/start sequences cannot
	// increase total occupancy past the first provisional interval's bound.
	retireOne(slot);
	auto entry = find(slot, index);
	if (entry != nil) {
		if (slot.entries[entry].owner == slot.active)
			return; // Preserve the first before-image across every short-loop wrap.
		removeRetired(slot, entry, true);
	}
	if (slot.active == nil)
		return;
	detail::invariant(slot.freeEntry != nil);
	entry = slot.freeEntry;
	slot.freeEntry = slot.entries[entry].next;
	auto& node = slot.entries[entry];
	node = {};
	node.key = index;
	node.original = original;
	node.owner = slot.active;
	++slot.descriptors[slot.active].count;
	append(slot, entry, slot.activeHead, slot.activeTail);
	slot.root = insert(slot, slot.root, entry);
	++work_.capturedFrames;
}

Frame GestureRollback::read(SlotId id, FrameIndex index, Frame backing) const {
	if (!initialized_ || !validSlot(id))
		return backing;
	const auto& slot = slots_[id];
	const auto entry = find(slot, index);
	if (entry != nil && slot.descriptors[slot.entries[entry].owner].resolution == Resolution::Cancelled)
		return slot.entries[entry].original;
	return backing;
}

Status GestureRollback::resolve(SlotId id, Resolution resolution) {
	if (!initialized_ || !validSlot(id))
		return Status::Invalid;
	auto& slot = slots_[id];
	if (slot.active == nil)
		return Status::Ok;
	auto& descriptor = slot.descriptors[slot.active];
	// This state change switches the entire effective mapping at this boundary.
	if (resolution == Resolution::Cancelled && slot.activeHead != nil)
		++slot.revision;
	descriptor.resolution = resolution;
	if (slot.activeHead == nil) {
		descriptor.next = slot.freeDescriptor;
		slot.freeDescriptor = slot.active;
	}
	else {
		if (slot.retiredTail == nil)
			slot.retiredHead = slot.activeHead;
		else
			slot.entries[slot.retiredTail].next = slot.activeHead;
		slot.entries[slot.activeHead].previous = slot.retiredTail;
		slot.retiredTail = slot.activeTail;
	}
	slot.active = slot.activeHead = slot.activeTail = nil;
	return Status::Ok;
}
Status GestureRollback::confirm(SlotId slot) {
	return resolve(slot, Resolution::Confirmed);
}
Status GestureRollback::cancel(SlotId slot) {
	return resolve(slot, Resolution::Cancelled);
}
void GestureRollback::advance(SampleClock now) {
	for (SlotId id = 0; id < kTrackCount; ++id) {
		auto& slot = slots_[id];
		slot.clock = std::max(slot.clock, now);
		if (slot.active != nil && slot.clock - slot.start >= kProvisionalSamples)
			confirm(id);
	}
}
void GestureRollback::rendered(SlotId id) {
	if (!initialized_ || !validSlot(id))
		return;
	auto& slot = slots_[id];
	if (slot.active != nil && ++slot.clock - slot.start >= kProvisionalSamples)
		confirm(id);
}
void GestureRollback::service(uint32_t maxFrames) {
	if (!initialized_)
		return;
	while (maxFrames) {
		bool found = false;
		for (unsigned n = 0; n < kTrackCount; ++n) {
			auto& slot = slots_[nextService_];
			nextService_ = (nextService_ + 1) % kTrackCount;
			if (slot.retiredHead != nil) {
				retireOne(slot);
				--maxFrames;
				found = true;
				break;
			}
		}
		if (!found)
			break;
	}
}
bool GestureRollback::pending(SlotId slot) const {
	return initialized_ && validSlot(slot) && slots_[slot].retiredHead != nil;
}
bool GestureRollback::provisional(SlotId slot) const {
	return initialized_ && validSlot(slot) && slots_[slot].active != nil;
}

} // namespace loop_station
