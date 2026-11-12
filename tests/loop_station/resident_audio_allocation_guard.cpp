#include "deluge/loop_station/audio_adapter.h"
#include "deluge/loop_station/gesture_rollback.h"
#include "deluge/loop_station/resident_audio.h"

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
	alignas(Frame) static std::byte scratch[GestureRollback::requiredBytes()]{};
	GestureRollback rollback(pool);
	ResidentAudio audio(pool, rollback);
	if (pool.initialize(storage) != Status::Ok || rollback.initialize(scratch) != Status::Ok)
		return 1;
	const Frame input[513] = {{42, -42}, {7, -7}};
	Frame output[513];
	const unsigned before = allocationCalls;
	if (pool.beginRenderRound(3) != Status::Ok || audio.beginInitial(0, 0) != Status::Ok
	    || audio.beginInitial(1, 0) != Status::Ok)
		return 2;
	for (SlotId slot = 0; slot < 2; ++slot) {
		audio.render(slot, input, output);
		if (audio.takeCompletion(slot).frames != 512)
			return 3;
		PhraseHandle handle;
		if (audio.finishInitial(slot, handle) != Status::Ok || audio.attach(slot, handle) != Status::Ok
		    || audio.start(slot, true) != Status::Ok || audio.setCaptureGate(slot, true) != Status::Ok)
			return 4;
		for (unsigned i = 0; i < 1000; ++i)
			audio.render(slot, input, output);
		if (audio.setCaptureGate(slot, false) != Status::Ok)
			return 5;
		audio.render(slot, input, output);
		if (audio.takeCompletion(slot).reason != CompletionReason::OneShot)
			return 6;
		audio.stop(slot);
	}
	Engine engine(audio, rollback);
	Transpose transpose;
	for (SlotId slot = 0; slot < kTrackCount; ++slot)
		if (transpose.prepare(slot) != Status::Ok)
			return 11;
	AudioAdapter adapter(engine, audio, pool, transpose);
	ClockMap clock;
	auto settings = defaultSettings();
	settings.startAction.mode = settings.stopAction.mode = ActionMode::Immediate;
	if (engine.dispatch(Command::configure(settings), 1000000, clock) != Status::Ok)
		return 8;
	for (SlotId slot = 2; slot < 4; ++slot)
		if (engine.dispatch({CommandType::Press, slot}, 1000000, clock) != Status::Ok)
			return 9;
	size_t count = 128;
	adapter.finalizeWindow(1000000, count, clock);
	for (SlotId slot = 2; slot < 4; ++slot) {
		adapter.renderDry(slot, std::span(input).first(count), std::span(output).first(count));
		if (engine.dispatch({CommandType::Press, slot}, 1000128, clock) != Status::Ok)
			return 10;
	}
	for (SlotId slot = 2; slot < 4; ++slot)
		if (engine.dispatch({CommandType::Press, slot}, 1020000, clock) != Status::Ok)
			return 10;
	for (unsigned window = 0; window < 1000; ++window) {
		count = 128;
		adapter.finalizeWindow(1020000 + window * count, count, clock);
		for (SlotId slot = 2; slot < 4; ++slot) {
			adapter.renderDry(slot, std::span(input).first(count), std::span(output).first(count));
			// Repeated render must not consume another frame or allocate a cache.
			adapter.renderDry(slot, std::span(input).first(count), std::span(output).first(count));
		}
	}
	return allocationCalls == before ? 0 : 7;
}
