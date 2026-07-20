#include "snapshot_comparison.h"
#include "snapshot_internal.h"

#include <assert.h>
#include <limits>
#include <map>
#include <utility>

namespace {

struct ComparedEntryAccounting
{
	const SnapshotEntry* sourceEntry = nullptr;
	std::optional<uint64_t> localAllocatedSize;
	std::optional<uint64_t> subtreeAllocatedSize;
	bool allocationOverflow = false;
};

using AccountingByPath = std::map<NativePath, ComparedEntryAccounting>;
using HardLinkGroupsByIdentity = std::map<thin_io::entry_identity, const SnapshotHardLinkGroup*, SnapshotInternal::EntryIdentityLess>;

void collectAccountingByPath(const SnapshotEntry& entry, const NativePath& path, AccountingByPath& accountingByPath)
{
	accountingByPath.emplace(path, ComparedEntryAccounting{&entry, entry.derived.localAllocatedSize});
	for (const auto& [name, child] : entry.children)
		collectAccountingByPath(child, appendNativeName(path, name), accountingByPath);
}

std::optional<NativePath> firstCommonAlias(const std::vector<NativePath>& left, const std::vector<NativePath>& right)
{
	auto leftAlias = left.begin();
	auto rightAlias = right.begin();
	while (leftAlias != left.end() && rightAlias != right.end())
	{
		if (*leftAlias < *rightAlias)
			++leftAlias;
		else if (*rightAlias < *leftAlias)
			++rightAlias;
		else
			return *leftAlias;
	}
	return {};
}

HardLinkGroupsByIdentity indexExactHardLinkGroups(const Snapshot& snapshot)
{
	HardLinkGroupsByIdentity groups;
	for (const SnapshotHardLinkGroup& group : snapshot.hardLinkGroups)
	{
		if (group.accountingExact)
			groups.emplace(group.identity, &group);
	}
	return groups;
}

std::optional<NativePath> firstCommonSingleLinkAlias(
	const SnapshotHardLinkGroup& group, const AccountingByPath& otherAccounting)
{
	for (const NativePath& alias : group.aliases)
	{
		const auto accounting = otherAccounting.find(alias);
		if (accounting == otherAccounting.end())
			continue;

		const SnapshotEntry& entry = *accounting->second.sourceEntry;
		if (entry.attributes.kind == thin_io::entry_kind::regular_file
			&& entry.metadata && entry.metadata->hardLinkCount == 1
			&& entry.metadata->identity && *entry.metadata->identity == group.identity
			&& accounting->second.localAllocatedSize)
		{
			return alias;
		}
	}
	return {};
}

void anchorHardLinkGroupAtAlias(
	const SnapshotHardLinkGroup& group, const NativePath& alias, AccountingByPath& accounting)
{
	for (const NativePath& groupAlias : group.aliases)
		accounting.at(groupAlias).localAllocatedSize = 0;
	accounting.at(alias).localAllocatedSize = group.allocatedSize;
}

void correlateHardLinkGroups(const Snapshot& baseline, const Snapshot& current,
	AccountingByPath& baselineAccounting, AccountingByPath& currentAccounting)
{
	const HardLinkGroupsByIdentity baselineGroups = indexExactHardLinkGroups(baseline);
	const HardLinkGroupsByIdentity currentGroups = indexExactHardLinkGroups(current);
	for (const SnapshotHardLinkGroup& baselineGroup : baseline.hardLinkGroups)
	{
		if (!baselineGroup.accountingExact)
			continue;
		const auto currentGroup = currentGroups.find(baselineGroup.identity);
		const std::optional<NativePath> commonAlias = currentGroup != currentGroups.end()
			? firstCommonAlias(baselineGroup.aliases, currentGroup->second->aliases)
			: firstCommonSingleLinkAlias(baselineGroup, currentAccounting);
		if (!commonAlias)
			continue;

		anchorHardLinkGroupAtAlias(baselineGroup, *commonAlias, baselineAccounting);
		if (currentGroup != currentGroups.end())
			anchorHardLinkGroupAtAlias(*currentGroup->second, *commonAlias, currentAccounting);
	}

	for (const SnapshotHardLinkGroup& currentGroup : current.hardLinkGroups)
	{
		if (!currentGroup.accountingExact || baselineGroups.contains(currentGroup.identity))
			continue;
		const std::optional<NativePath> commonAlias = firstCommonSingleLinkAlias(currentGroup, baselineAccounting);
		if (commonAlias)
			anchorHardLinkGroupAtAlias(currentGroup, *commonAlias, currentAccounting);
	}
}

void recalculateSubtreeAccounting(const SnapshotEntry& entry, const NativePath& path, AccountingByPath& accountingByPath)
{
	ComparedEntryAccounting& accounting = accountingByPath.at(path);
	accounting.allocationOverflow = false;
	std::optional<uint64_t> subtreeAllocatedSize = accounting.localAllocatedSize;
	for (const auto& [name, child] : entry.children)
	{
		const NativePath childPath = appendNativeName(path, name);
		recalculateSubtreeAccounting(child, childPath, accountingByPath);
		subtreeAllocatedSize = SnapshotInternal::addAllocatedSizes(
			subtreeAllocatedSize, accountingByPath.at(childPath).subtreeAllocatedSize, accounting.allocationOverflow);
	}

	if (!entry.derived.subtreeCoverageComplete)
		subtreeAllocatedSize.reset();
	accounting.subtreeAllocatedSize = subtreeAllocatedSize;
}

std::pair<AccountingByPath, AccountingByPath> buildComparisonAccounting(const Snapshot& baseline, const Snapshot& current)
{
	AccountingByPath baselineAccounting;
	AccountingByPath currentAccounting;
	collectAccountingByPath(baseline.root, baseline.rootPath, baselineAccounting);
	collectAccountingByPath(current.root, current.rootPath, currentAccounting);
	correlateHardLinkGroups(baseline, current, baselineAccounting, currentAccounting);
	recalculateSubtreeAccounting(baseline.root, baseline.rootPath, baselineAccounting);
	recalculateSubtreeAccounting(current.root, current.rootPath, currentAccounting);
	return {std::move(baselineAccounting), std::move(currentAccounting)};
}

bool isValidComparisonRoot(const Snapshot& snapshot)
{
	return !snapshot.rootPath.isEmpty()
		&& snapshot.root.attributes.kind == thin_io::entry_kind::directory
		&& !snapshot.root.attributes.is_link
		&& snapshot.root.metadata.has_value()
		&& snapshot.root.traversalState == DirectoryTraversalState::completed;
}

std::optional<thin_io::filesystem_identity> filesystemIdentity(const Snapshot& snapshot)
{
	if (snapshot.filesystemSpaceAtCompletion && snapshot.filesystemSpaceAtCompletion->identity)
		return snapshot.filesystemSpaceAtCompletion->identity;
	if (snapshot.filesystemSpaceAtStart && snapshot.filesystemSpaceAtStart->identity)
		return snapshot.filesystemSpaceAtStart->identity;
	if (snapshot.root.metadata->identity)
		return snapshot.root.metadata->identity->filesystem;
	return {};
}

MagnitudeChange magnitudeChange(const uint64_t baseline, const uint64_t current)
{
	if (current > baseline)
		return {ChangeDirection::increase, current - baseline};
	if (current < baseline)
		return {ChangeDirection::decrease, baseline - current};
	return {};
}

MagnitudeChange inverted(MagnitudeChange change)
{
	if (change.direction == ChangeDirection::increase)
		change.direction = ChangeDirection::decrease;
	else if (change.direction == ChangeDirection::decrease)
		change.direction = ChangeDirection::increase;
	return change;
}

std::optional<MagnitudeChange> addMagnitudeChanges(const MagnitudeChange& left, const MagnitudeChange& right)
{
	if (left.direction == ChangeDirection::unchanged)
		return right;
	if (right.direction == ChangeDirection::unchanged)
		return left;
	if (left.direction == right.direction)
	{
		if (right.magnitude > std::numeric_limits<uint64_t>::max() - left.magnitude)
			return {};
		return MagnitudeChange{left.direction, left.magnitude + right.magnitude};
	}
	if (left.magnitude > right.magnitude)
		return MagnitudeChange{left.direction, left.magnitude - right.magnitude};
	if (right.magnitude > left.magnitude)
		return MagnitudeChange{right.direction, right.magnitude - left.magnitude};
	return MagnitudeChange{};
}

void deriveSpaceSummary(const Snapshot& baseline, const Snapshot& current, ComparisonSummary& summary)
{
	if (baseline.filesystemSpaceAtCompletion && current.filesystemSpaceAtCompletion)
	{
		summary.freeSpaceChange = magnitudeChange(baseline.filesystemSpaceAtCompletion->free, current.filesystemSpaceAtCompletion->free);
		summary.availableSpaceChange = magnitudeChange(
			baseline.filesystemSpaceAtCompletion->available, current.filesystemSpaceAtCompletion->available);
		summary.capacityChange = magnitudeChange(
			baseline.filesystemSpaceAtCompletion->capacity, current.filesystemSpaceAtCompletion->capacity);
	}
	if (baseline.filesystemSpaceAtStart && baseline.filesystemSpaceAtCompletion)
		summary.baselineScanFreeSpaceChange = magnitudeChange(
			baseline.filesystemSpaceAtStart->free, baseline.filesystemSpaceAtCompletion->free);
	if (current.filesystemSpaceAtStart && current.filesystemSpaceAtCompletion)
		summary.currentScanFreeSpaceChange = magnitudeChange(
			current.filesystemSpaceAtStart->free, current.filesystemSpaceAtCompletion->free);
}

struct ComparisonSide
{
	const SnapshotEntry* entry = nullptr;
	bool absenceAuthoritative = false;
	const AccountingByPath* accountingByPath = nullptr;
};

std::optional<uint64_t> localAllocatedSize(const ComparisonSide& side, const NativePath& path)
{
	if (side.entry)
		return side.accountingByPath->at(path).localAllocatedSize;
	if (side.absenceAuthoritative)
		return 0;
	return {};
}

std::optional<uint64_t> subtreeAllocatedSize(const ComparisonSide& side, const NativePath& path)
{
	if (side.entry)
		return side.accountingByPath->at(path).subtreeAllocatedSize;
	if (side.absenceAuthoritative)
		return 0;
	return {};
}

bool childrenAreAuthoritative(const ComparisonSide& side)
{
	if (!side.entry)
		return side.absenceAuthoritative;
	if (side.entry->attributes.kind != thin_io::entry_kind::directory)
		return true;
	return side.entry->traversalState == DirectoryTraversalState::completed
		|| side.entry->traversalState == DirectoryTraversalState::link_boundary
		|| side.entry->traversalState == DirectoryTraversalState::mount_boundary;
}

bool allocationOverflowed(const ComparisonSide& side, const NativePath& path)
{
	return side.entry && side.accountingByPath->at(path).allocationOverflow;
}

ComparisonExcludedRegion excludedRegion(const NativePath& path, const ComparisonSide& baseline, const ComparisonSide& current)
{
	const bool baselineChildrenAuthoritative = childrenAreAuthoritative(baseline);
	const bool currentChildrenAuthoritative = childrenAreAuthoritative(current);
	const std::optional<uint64_t> baselineLocalSize = localAllocatedSize(baseline, path);
	const std::optional<uint64_t> currentLocalSize = localAllocatedSize(current, path);

	ComparisonExcludedRegion region;
	region.path = path;
	region.baselineCoverageIncomplete = !baselineChildrenAuthoritative
		|| (!baseline.entry && !baseline.absenceAuthoritative)
		|| (baseline.entry && !baseline.entry->derived.localCoverageComplete);
	region.currentCoverageIncomplete = !currentChildrenAuthoritative
		|| (!current.entry && !current.absenceAuthoritative)
		|| (current.entry && !current.entry->derived.localCoverageComplete);
	region.baselineAccountingUncertain = allocationOverflowed(baseline, path)
		|| (!baselineLocalSize && !region.baselineCoverageIncomplete);
	region.currentAccountingUncertain = allocationOverflowed(current, path)
		|| (!currentLocalSize && !region.currentCoverageIncomplete);
	return region;
}

void compareEntries(const ComparisonSide& baseline, const ComparisonSide& current, const NativePath& path,
	const uint64_t threshold, SnapshotComparisonResult& result)
{
	const std::optional<uint64_t> baselineSubtreeSize = subtreeAllocatedSize(baseline, path);
	const std::optional<uint64_t> currentSubtreeSize = subtreeAllocatedSize(current, path);
	const bool baselineChildrenAuthoritative = childrenAreAuthoritative(baseline);
	const bool currentChildrenAuthoritative = childrenAreAuthoritative(current);

	const bool localOrChildSetIsUnknown = !localAllocatedSize(baseline, path)
		|| !localAllocatedSize(current, path)
		|| !baselineChildrenAuthoritative
		|| !currentChildrenAuthoritative
		|| allocationOverflowed(baseline, path)
		|| allocationOverflowed(current, path);
	if ((!baselineSubtreeSize || !currentSubtreeSize) && localOrChildSetIsUnknown)
		result.excludedRegions.push_back(excludedRegion(path, baseline, current));

	const size_t changesBeforeChildren = result.changes.size();
	auto compareChild = [&](const NativeName& name, const SnapshotEntry* baselineChild, const SnapshotEntry* currentChild)
	{
		if ((!baselineChild && !baselineChildrenAuthoritative) || (!currentChild && !currentChildrenAuthoritative))
			return;

		const NativePath childPath = appendNativeName(path, name);
		compareEntries(
			{baselineChild, !baselineChild && baselineChildrenAuthoritative, baseline.accountingByPath},
			{currentChild, !currentChild && currentChildrenAuthoritative, current.accountingByPath},
			childPath, threshold, result);
	};

	using Children = decltype(SnapshotEntry::children);
	static const Children noChildren;
	const Children& baselineChildren = baseline.entry ? baseline.entry->children : noChildren;
	const Children& currentChildren = current.entry ? current.entry->children : noChildren;
	auto baselineChild = baselineChildren.begin();
	const auto baselineChildrenEnd = baselineChildren.end();
	auto currentChild = currentChildren.begin();
	const auto currentChildrenEnd = currentChildren.end();
	while (baselineChild != baselineChildrenEnd || currentChild != currentChildrenEnd)
	{
		if (currentChild == currentChildrenEnd
			|| (baselineChild != baselineChildrenEnd && baselineChildren.key_comp()(baselineChild.key(), currentChild.key())))
		{
			compareChild(baselineChild.key(), &baselineChild.value(), nullptr);
			++baselineChild;
		}
		else if (baselineChild == baselineChildrenEnd || currentChildren.key_comp()(currentChild.key(), baselineChild.key()))
		{
			compareChild(currentChild.key(), nullptr, &currentChild.value());
			++currentChild;
		}
		else
		{
			compareChild(baselineChild.key(), &baselineChild.value(), &currentChild.value());
			++baselineChild;
			++currentChild;
		}
	}

	if (!baselineSubtreeSize || !currentSubtreeSize || *currentSubtreeSize <= *baselineSubtreeSize)
		return;
	const uint64_t allocatedIncrease = *currentSubtreeSize - *baselineSubtreeSize;
	if (allocatedIncrease >= threshold && result.changes.size() == changesBeforeChildren)
		result.changes.push_back({path, allocatedIncrease});
}

} // namespace

std::expected<SnapshotComparisonResult, SnapshotComparisonError> compareSnapshots(
	const Snapshot& baseline, const Snapshot& current, const uint64_t allocatedIncreaseThreshold)
{
	if (!isValidComparisonRoot(baseline))
		return std::unexpected{SnapshotComparisonError::invalid_baseline_root};
	if (!isValidComparisonRoot(current))
		return std::unexpected{SnapshotComparisonError::invalid_current_root};
	assert(baseline.derivedDataAvailable);
	assert(current.derivedDataAvailable);
	if (baseline.rootPath != current.rootPath)
		return std::unexpected{SnapshotComparisonError::different_root_paths};

	SnapshotComparisonResult result;
	const std::optional<thin_io::filesystem_identity> baselineFilesystemIdentity = filesystemIdentity(baseline);
	const std::optional<thin_io::filesystem_identity> currentFilesystemIdentity = filesystemIdentity(current);
	if (baselineFilesystemIdentity && currentFilesystemIdentity)
	{
		if (*baselineFilesystemIdentity != *currentFilesystemIdentity)
			return std::unexpected{SnapshotComparisonError::filesystem_identity_mismatch};
	}
	else
	{
		result.warnings.push_back(SnapshotComparisonWarning::filesystem_identity_unavailable);
	}

	const std::optional<thin_io::entry_identity>& baselineRootIdentity = baseline.root.metadata->identity;
	const std::optional<thin_io::entry_identity>& currentRootIdentity = current.root.metadata->identity;
	if (baselineRootIdentity && currentRootIdentity)
	{
		if (*baselineRootIdentity != *currentRootIdentity)
			return std::unexpected{SnapshotComparisonError::root_identity_mismatch};
	}
	else
	{
		result.warnings.push_back(SnapshotComparisonWarning::root_identity_unavailable);
	}

	auto [baselineAccounting, currentAccounting] = buildComparisonAccounting(baseline, current);
	deriveSpaceSummary(baseline, current, result.summary);
	const std::optional<uint64_t> baselineAllocatedSize = baselineAccounting.at(baseline.rootPath).subtreeAllocatedSize;
	const std::optional<uint64_t> currentAllocatedSize = currentAccounting.at(current.rootPath).subtreeAllocatedSize;
	if (baselineAllocatedSize && currentAllocatedSize)
		result.summary.allocatedTreeChange = magnitudeChange(*baselineAllocatedSize, *currentAllocatedSize);

	if (result.summary.freeSpaceChange && result.summary.allocatedTreeChange)
	{
		const MagnitudeChange filesystemConsumptionChange = inverted(*result.summary.freeSpaceChange);
		const std::optional<MagnitudeChange> unexplained = addMagnitudeChanges(
			filesystemConsumptionChange, inverted(*result.summary.allocatedTreeChange));
		if (unexplained)
		{
			result.summary.unexplainedConsumptionChange = unexplained;
			result.summary.reconciliation = ReconciliationState::exact;
		}
		else
		{
			result.summary.reconciliation = ReconciliationState::overflow;
		}
	}

	compareEntries(
		{&baseline.root, false, &baselineAccounting},
		{&current.root, false, &currentAccounting},
		baseline.rootPath, allocatedIncreaseThreshold, result);
	return result;
}
