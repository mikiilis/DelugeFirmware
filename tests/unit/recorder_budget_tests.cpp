#include "CppUTest/TestHarness.h"

#if __has_include("processing/engines/recorder_budget.h")
#include "processing/engines/recorder_budget.h"
#include <array>
#include <optional>
#include <utility>
#endif

TEST_GROUP(RecorderBudget){};

#if __has_include("processing/engines/recorder_budget.h")

TEST(RecorderBudget, BoundsRetainedObjectsSeparatelyFromCaptureChannels) {
	deluge::RecorderBudget budget;
	std::array<std::optional<deluge::RecorderBudget::Lease>, 8> sealed;
	std::array<std::optional<deluge::RecorderBudget::Lease>, 8> active;

	for (auto& lease : sealed) {
		lease = budget.tryAcquire(2, true);
		CHECK(lease.has_value());
		lease->finishCapture();
	}
	for (auto& lease : active) {
		lease = budget.tryAcquire(2, true);
		CHECK(lease.has_value());
	}

	CHECK_EQUAL(16, budget.retainedCount());
	CHECK_EQUAL(16, budget.channelUsage());
	auto rejected = budget.tryAcquire(1, true);
	CHECK_FALSE(rejected.has_value());
	CHECK_EQUAL(16, budget.retainedCount());
	CHECK_EQUAL(16, budget.channelUsage());
}

TEST(RecorderBudget, OrdinaryRecorderKeepsItsChannelChargeUntilDestruction) {
	deluge::RecorderBudget budget;
	{
		auto lease = budget.tryAcquire(2, false);
		CHECK(lease.has_value());
		lease->finishCapture();
		CHECK_EQUAL(1, budget.retainedCount());
		CHECK_EQUAL(2, budget.channelUsage());
	}
	CHECK_EQUAL(0, budget.retainedCount());
	CHECK_EQUAL(0, budget.channelUsage());
}

TEST(RecorderBudget, ReservationIsVisibleToRecursiveAdmissionDuringSetup) {
	deluge::RecorderBudget budget;
	{
		auto setupLease = budget.tryAcquire(2, true);
		CHECK(setupLease.has_value());
		auto nestedLease = budget.tryAcquire(2, true);
		CHECK(nestedLease.has_value());
		CHECK_EQUAL(2, budget.retainedCount());
		CHECK_EQUAL(4, budget.channelUsage());
	}
	CHECK_EQUAL(0, budget.retainedCount());
	CHECK_EQUAL(0, budget.channelUsage());
}

TEST(RecorderBudget, MovingLeaseReleasesItsChargesExactlyOnce) {
	deluge::RecorderBudget budget;
	{
		auto acquired = budget.tryAcquire(2, true);
		CHECK(acquired.has_value());
		auto moved = std::move(*acquired);
		acquired.reset();
		CHECK_EQUAL(1, budget.retainedCount());
		CHECK_EQUAL(2, budget.channelUsage());
		moved.finishCapture();
		CHECK_EQUAL(1, budget.retainedCount());
		CHECK_EQUAL(0, budget.channelUsage());
	}
	CHECK_EQUAL(0, budget.retainedCount());
	CHECK_EQUAL(0, budget.channelUsage());
}

#endif
