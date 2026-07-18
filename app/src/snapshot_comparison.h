#pragma once

#include "snapshot.h"

#include <expected>
#include <optional>
#include <stdint.h>
#include <vector>

enum class ChangeDirection : uint8_t {
	unchanged,
	increase,
	decrease
};

struct MagnitudeChange
{
	ChangeDirection direction = ChangeDirection::unchanged;
	uint64_t magnitude = 0;

	[[nodiscard]] bool operator==(const MagnitudeChange&) const = default;
};

enum class SnapshotComparisonError : uint8_t {
	invalid_baseline_root,
	invalid_current_root,
	different_root_paths,
	filesystem_identity_mismatch,
	root_identity_mismatch
};

enum class SnapshotComparisonWarning : uint8_t {
	root_identity_unavailable,
	filesystem_identity_unavailable
};

enum class ReconciliationState : uint8_t {
	exact,
	incomplete,
	overflow
};

struct ComparisonSummary
{
	std::optional<MagnitudeChange> freeSpaceChange;
	std::optional<MagnitudeChange> availableSpaceChange;
	std::optional<MagnitudeChange> capacityChange;
	std::optional<MagnitudeChange> baselineScanFreeSpaceChange;
	std::optional<MagnitudeChange> currentScanFreeSpaceChange;
	std::optional<MagnitudeChange> allocatedTreeChange;
	std::optional<MagnitudeChange> unexplainedConsumptionChange;
	ReconciliationState reconciliation = ReconciliationState::incomplete;

	[[nodiscard]] bool operator==(const ComparisonSummary&) const = default;
};

struct ComparisonChange
{
	NativePath path;
	uint64_t allocatedIncrease = 0;

	[[nodiscard]] bool operator==(const ComparisonChange&) const = default;
};

struct ComparisonExcludedRegion
{
	NativePath path;
	bool baselineCoverageIncomplete = false;
	bool baselineAccountingUncertain = false;
	bool currentCoverageIncomplete = false;
	bool currentAccountingUncertain = false;

	[[nodiscard]] bool operator==(const ComparisonExcludedRegion&) const = default;
};

struct SnapshotComparisonResult
{
	std::vector<SnapshotComparisonWarning> warnings;
	ComparisonSummary summary;
	std::vector<ComparisonChange> changes;
	std::vector<ComparisonExcludedRegion> excludedRegions;
};

[[nodiscard]] std::expected<SnapshotComparisonResult, SnapshotComparisonError> compareSnapshots(
	const Snapshot& baseline, const Snapshot& current, uint64_t allocatedIncreaseThreshold);
