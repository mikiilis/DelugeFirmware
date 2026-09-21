/* Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

namespace deluge::loop_station {
enum class StorageStage : uint8_t { NONE, SEEK, SHORT_SEEK, WRITE, SHORT_WRITE, SYNC, CLOSE };
struct StorageFailure {
	StorageStage stage = StorageStage::NONE;
	int32_t rawResult = 0;
	uint32_t offset = 0;
	uint32_t requested = 0;
	uint32_t actual = 0;
};
// The caller pins the snapshot and recorder across every yielding operation.
// Partial sectors must reach the disk before readers can use a raw SD address.
template <typename File, typename Aborted>
StorageFailure writeLoopSnapshot(File& file, uint32_t offset, std::span<std::byte> data, Aborted aborted) {
	auto seek = file.lseek(offset);
	if (aborted())
		return {};
	if (!seek)
		return {StorageStage::SEEK, static_cast<int32_t>(seek.error()), offset, uint32_t(data.size()), 0};
	if (file.inner().fptr != offset)
		return {StorageStage::SHORT_SEEK, 0, offset, uint32_t(data.size()), uint32_t(file.inner().fptr)};
	auto written = file.write(data);
	if (aborted())
		return {};
	if (!written)
		return {StorageStage::WRITE, static_cast<int32_t>(written.error()), offset, uint32_t(data.size()), 0};
	if (*written != data.size())
		return {StorageStage::SHORT_WRITE, 0, offset, uint32_t(data.size()), uint32_t(*written)};
	if ((offset + data.size()) % 512) {
		auto synced = file.sync();
		if (aborted())
			return {};
		if (!synced)
			return {StorageStage::SYNC, static_cast<int32_t>(synced.error()), offset, uint32_t(data.size()),
			        uint32_t(*written)};
	}
	return {};
}
} // namespace deluge::loop_station
