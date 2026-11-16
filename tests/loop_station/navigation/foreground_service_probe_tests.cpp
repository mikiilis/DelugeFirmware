#include <cstdint>
#include <cstdio>

namespace {
void check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}
} // namespace

int main() {
	using loop_station::PersistenceState;
	using loop_station::Status;

	ProbeSong song;
	// With no loop station owner the foreground tick is inert.
	song.serviceLoopStationArchive(4096);
	check(song.persistence.candidateCalls == 0, "no owner leaves the foreground tick inert");

	ProbeOwner owner;
	song.loopStationOwner_ = &owner;
	// The archive tick must service the confirmed history navigation with the
	// bounded 128-frame budget even though no SD work is pending here.
	song.serviceLoopStationArchive(4096);
	check(owner.model.archiveCalls == 1, "each foreground tick runs one archive quantum");
	check(song.persistence.candidateCalls == 1, "each foreground tick runs one candidate review");
	check(owner.model.navigationCalls == 1, "the production foreground tick services confirmed history navigation");
	check(owner.model.navigationBudget == 128, "confirmed history navigation runs on the bounded 128-frame budget");
	// Even with no SD progress the navigation keeps being serviced.
	song.serviceLoopStationArchive(0);
	check(owner.model.navigationCalls == 2, "navigation progresses on a later foreground tick without SD work");

	// N5: the extracted FirmwareOwner mask accepts a two-slot checkpoint load,
	// keeps slot 0 active across ticks while its real read is Busy, then advances
	// to slot 1 after exactly one successful commit. Repeated ticks model a
	// harmless root-exit/return; the owner does not drop or duplicate the request.
	PersistenceState checkpoints;
	checkpoints.admission.admitted[0] = true;
	checkpoints.admission.phrases[0].identity = 11;
	checkpoints.admission.admitted[1] = true;
	checkpoints.admission.phrases[1].identity = 22;
	owner.beginCheckpointRestores(checkpoints);
	check(owner.restoreMask == 0x03, "both admitted checkpoints remain in the restore mask");
	check(owner.activeRestoreSlot == 0, "checkpoint restore starts at the first admitted slot");
	check(owner.model.requestCalls[0] == 1, "slot 0 checkpoint request is accepted once");
	check(owner.model.requestCalls[1] == 0, "slot 1 is not requested concurrently");
	owner.model.serviceStatus = Status::Busy;
	for (unsigned tick = 0; tick < 3; ++tick)
		song.serviceLoopStationArchive(4096);
	check(owner.model.serviceCalls == 3, "each foreground tick services the accepted checkpoint read");
	check(owner.activeRestoreSlot == 0, "a Busy real read survives repeated foreground ticks");
	check(owner.restoreMask == 0x03, "Busy does not clear the accepted mask bit");
	check(owner.model.requestCalls[0] == 1, "Busy does not duplicate the slot 0 request");

	owner.model.serviceStatus = Status::Ok;
	owner.model.commitStatus = Status::Ok;
	song.serviceLoopStationArchive(4096);
	check(owner.model.commitCalls == 1, "the completed read commits exactly once");
	check((owner.restoreMask & 0x01) == 0, "a committed checkpoint leaves the mask");
	check(owner.activeRestoreSlot == 1, "the owner advances to the next admitted checkpoint");
	check(owner.model.requestCalls[1] == 1, "slot 1 checkpoint is requested after slot 0 completes");

	// A later real read failure cancels that slot, clears its bit, and never
	// retries it; the remaining mask entries still advance.
	owner.model.serviceStatus = Status::Stale;
	song.serviceLoopStationArchive(4096);
	check(owner.model.cancelCalls[1] == 1, "a failed checkpoint read is cancelled explicitly");
	check(owner.restoreMask == 0, "a cancelled checkpoint leaves the mask");
	check(!owner.activeRestore.valid(), "a cancelled checkpoint leaves no active request");
	check(owner.model.requestCalls[1] == 1, "a cancelled checkpoint is never re-requested");

	// A request that cannot even be opened is dropped and does not block a later
	// admitted slot.
	ProbeOwner requestFailure;
	PersistenceState later;
	later.admission.admitted[0] = true;
	later.admission.phrases[0].identity = 31;
	later.admission.admitted[1] = true;
	later.admission.phrases[1].identity = 32;
	requestFailure.model.requestStatus[0] = Status::NoMemory;
	requestFailure.beginCheckpointRestores(later);
	check(requestFailure.model.requestCalls[0] == 1, "the unavailable first request is attempted once");
	check((requestFailure.restoreMask & 0x01) == 0, "an unavailable first request is removed from the mask");
	check(requestFailure.activeRestoreSlot == 1, "the owner advances past an unavailable checkpoint");
	check(requestFailure.model.requestCalls[1] == 1, "the next admitted checkpoint is requested");
	return 0;
}
