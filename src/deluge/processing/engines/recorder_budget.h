/*
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <cstdint>
#include <optional>
#include <utility>

namespace deluge {

// Admission is reserved before recorder allocation/setup can yield. Retained
// objects bound delayed writers and playback providers; channel units bound
// only active capture for Loop Station recorders.
class RecorderBudget {
public:
	static constexpr uint8_t kMaxRetainedRecorders = 16;
	static constexpr uint8_t kMaxChannelUnits = 16;

	class Lease {
	public:
		Lease() = default;
		Lease(Lease const&) = delete;
		Lease& operator=(Lease const&) = delete;

		Lease(Lease&& other) noexcept { take(other); }
		Lease& operator=(Lease&& other) noexcept {
			if (this != &other) {
				release();
				take(other);
			}
			return *this;
		}

		~Lease() { release(); }

		// A Loop Station writer stops consuming input channels when capture ends,
		// but keeps its retained-object reservation until the recorder is deleted.
		// Ordinary recorder accounting remains charged through destruction.
		void finishCapture() noexcept {
			if (budget_ && loopStation_ && channelChargeActive_) {
				budget_->channelUsage_ -= channels_;
				channelChargeActive_ = false;
			}
		}

		[[nodiscard]] explicit operator bool() const noexcept { return budget_ != nullptr; }

	private:
		friend class RecorderBudget;
		Lease(RecorderBudget& budget, uint8_t channels, bool loopStation) noexcept
		    : budget_(&budget), channels_(channels), loopStation_(loopStation), channelChargeActive_(true) {}

		void take(Lease& other) noexcept {
			budget_ = std::exchange(other.budget_, nullptr);
			channels_ = std::exchange(other.channels_, 0);
			loopStation_ = std::exchange(other.loopStation_, false);
			channelChargeActive_ = std::exchange(other.channelChargeActive_, false);
		}

		void release() noexcept {
			if (!budget_) {
				return;
			}
			if (channelChargeActive_) {
				budget_->channelUsage_ -= channels_;
			}
			--budget_->retainedCount_;
			budget_ = nullptr;
			channels_ = 0;
			channelChargeActive_ = false;
		}

		RecorderBudget* budget_ = nullptr;
		uint8_t channels_ = 0;
		bool loopStation_ = false;
		bool channelChargeActive_ = false;
	};

	[[nodiscard]] bool canAcquire(uint8_t channels) const noexcept {
		return channels && channels <= 2 && retainedCount_ < kMaxRetainedRecorders
		       && channelUsage_ + channels <= kMaxChannelUnits;
	}

	[[nodiscard]] std::optional<Lease> tryAcquire(uint8_t channels, bool loopStation) noexcept {
		if (!canAcquire(channels)) {
			return std::nullopt;
		}
		++retainedCount_;
		channelUsage_ += channels;
		return Lease(*this, channels, loopStation);
	}

	[[nodiscard]] uint8_t retainedCount() const noexcept { return retainedCount_; }
	[[nodiscard]] uint8_t channelUsage() const noexcept { return channelUsage_; }

private:
	uint8_t retainedCount_ = 0;
	uint8_t channelUsage_ = 0;
};

} // namespace deluge
