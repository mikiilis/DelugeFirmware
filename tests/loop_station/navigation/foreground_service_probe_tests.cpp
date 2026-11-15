namespace {
void check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "FAIL: %s\n", message);
		std::abort();
	}
}
} // namespace
int main() {
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
	check(owner.restoreCalls == 1, "the same foreground tick keeps checkpoint restore serialization");
	// Even with no SD progress the navigation keeps being serviced.
	song.serviceLoopStationArchive(0);
	check(owner.model.navigationCalls == 2, "navigation progresses on a later foreground tick without SD work");
	return 0;
}
