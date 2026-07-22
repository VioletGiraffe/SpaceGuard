#include "3rdparty/catch2/catch.hpp"

#include "snapshot_comparison.h"

#include <QTimeZone>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

NativePath nativePath(const char* value)
{
#ifdef _WIN32
	return QString::fromUtf8(value);
#else
	return QByteArray{value};
#endif
}

NativeName nativeName(const char* value)
{
	return nativePath(value);
}

thin_io::entry_identity entryIdentity(const uint64_t filesystem, const uint8_t value)
{
	thin_io::entry_identity identity;
	identity.filesystem = filesystem;
	identity.entry.fill(value);
	return identity;
}

SnapshotEntryMetadata entryMetadata(const uint64_t allocatedSize, const uint64_t hardLinkCount = 1,
	std::optional<thin_io::entry_identity> identity = {})
{
	return {allocatedSize, allocatedSize, hardLinkCount, std::move(identity)};
}

SnapshotEntry regularFile(const uint64_t allocatedSize, const uint64_t hardLinkCount = 1,
	std::optional<thin_io::entry_identity> identity = {})
{
	SnapshotEntry entry;
	entry.attributes.kind = thin_io::entry_kind::regular_file;
	entry.metadata = entryMetadata(allocatedSize, hardLinkCount, std::move(identity));
	return entry;
}

SnapshotEntry directory(const DirectoryTraversalState state = DirectoryTraversalState::completed)
{
	SnapshotEntry entry;
	entry.attributes.kind = thin_io::entry_kind::directory;
	entry.metadata = entryMetadata(0);
	entry.traversalState = state;
	return entry;
}

Snapshot makeSnapshot()
{
	constexpr uint64_t Filesystem = 42;
	Snapshot snapshot;
#ifdef _WIN32
	snapshot.rootPath = R"(C:\root)";
#else
	snapshot.rootPath = "/root";
#endif
	snapshot.root = directory();
	snapshot.root.metadata->identity = entryIdentity(Filesystem, 1);
	snapshot.filesystemSpaceAtStart = thin_io::filesystem_space{1000, 510, 410, Filesystem};
	snapshot.filesystemSpaceAtCompletion = thin_io::filesystem_space{1000, 500, 400, Filesystem};
	snapshot.scanStartedAtUtc = QDateTime::fromMSecsSinceEpoch(1000, QTimeZone::UTC);
	snapshot.scanCompletedAtUtc = QDateTime::fromMSecsSinceEpoch(2000, QTimeZone::UTC);
	return snapshot;
}

NativePath childPath(const NativePath& parent, const char* child)
{
	return appendNativeName(parent, nativeName(child));
}

const ComparisonChange* findChange(const SnapshotComparisonResult& result, const NativePath& path)
{
	const auto change = std::ranges::find_if(result.changes, [&path](const ComparisonChange& candidate) { return candidate.path == path; });
	return change != result.changes.end() ? &*change : nullptr;
}

ComparisonChange expectedChange(const NativePath& path, const uint64_t baselineSubtreeAllocatedSize, const uint64_t currentSubtreeAllocatedSize,
	const thin_io::entry_kind currentEntryKind, const bool baselineEntryExists)
{
	ComparisonChange change;
	change.path = path;
	change.baselineSubtreeAllocatedSize = baselineSubtreeAllocatedSize;
	change.currentSubtreeAllocatedSize = currentSubtreeAllocatedSize;
	change.allocatedIncrease = currentSubtreeAllocatedSize - baselineSubtreeAllocatedSize;
	change.currentEntryKind = currentEntryKind;
	change.baselineEntryExists = baselineEntryExists;
	return change;
}

const ComparisonExcludedRegion* findExcludedRegion(const SnapshotComparisonResult& result, const NativePath& path)
{
	const auto region = std::ranges::find_if(
		result.excludedRegions, [&path](const ComparisonExcludedRegion& candidate) { return candidate.path == path; });
	return region != result.excludedRegions.end() ? &*region : nullptr;
}

std::expected<SnapshotComparisonResult, SnapshotComparisonError> comparePrepared(
	Snapshot& baseline, Snapshot& current, const uint64_t threshold)
{
	baseline.rebuildDerivedData();
	current.rebuildDerivedData();
	return compareSnapshots(baseline, current, threshold);
}

} // namespace

TEST_CASE("Derived accounting groups hard links deterministically", "[snapshot][accounting]")
{
	Snapshot snapshot = makeSnapshot();
	SnapshotEntry firstDirectory = directory();
	SnapshotEntry secondDirectory = directory();
	const thin_io::entry_identity identity = entryIdentity(42, 9);
	firstDirectory.children.try_emplace(nativeName("z"), regularFile(100, 2, identity));
	secondDirectory.children.try_emplace(nativeName("a"), regularFile(100, 2, identity));
	snapshot.root.children.try_emplace(nativeName("a"), std::move(firstDirectory));
	snapshot.root.children.try_emplace(nativeName("b"), std::move(secondDirectory));

	snapshot.rebuildDerivedData();
	REQUIRE(snapshot.derivedDataAvailable);
	REQUIRE(snapshot.hardLinkGroups.size() == 1);
	const SnapshotHardLinkGroup& group = snapshot.hardLinkGroups.front();
	CHECK(group.metadataConsistent);
	CHECK(group.allAliasesObserved);
	CHECK(group.accountingExact);
	CHECK(group.presentationPath == childPath(childPath(snapshot.rootPath, "a"), "z"));
	CHECK(snapshot.root.derived.subtreeAllocatedSize == 100);
	CHECK(snapshot.root.derived.knownSubtreeAllocatedSizeLowerBound == 100);
	CHECK(snapshot.root.children.at(nativeName("a")).children.at(nativeName("z")).derived.localAllocatedSize == 100);
	CHECK(snapshot.root.children.at(nativeName("b")).children.at(nativeName("a")).derived.localAllocatedSize == 0);
}

TEST_CASE("Derived accounting bypasses hard-link grouping for one-link files", "[snapshot][accounting]")
{
	Snapshot snapshot = makeSnapshot();
	snapshot.root.children.try_emplace(nativeName("file"), regularFile(100, 1, entryIdentity(42, 9)));

	snapshot.rebuildDerivedData();
	CHECK(snapshot.hardLinkGroups.empty());
	CHECK(snapshot.root.children.at(nativeName("file")).derived.localAllocatedSize == 100);
	CHECK(snapshot.root.derived.subtreeAllocatedSize == 100);
	CHECK(snapshot.root.derived.knownSubtreeAllocatedSizeLowerBound == 100);
}

TEST_CASE("Derived accounting retains known allocation below incomplete subtrees", "[snapshot][accounting]")
{
	Snapshot snapshot = makeSnapshot();
	SnapshotEntry partialDirectory = directory();
	partialDirectory.children.try_emplace(nativeName("known"), regularFile(100));
	partialDirectory.children.try_emplace(nativeName("incomplete"), directory(DirectoryTraversalState::enumeration_failed));
	snapshot.root.children.try_emplace(nativeName("partial"), std::move(partialDirectory));
	SnapshotEntry unknownFile;
	unknownFile.attributes.kind = thin_io::entry_kind::regular_file;
	snapshot.root.children.try_emplace(nativeName("unknown"), std::move(unknownFile));

	snapshot.rebuildDerivedData();
	CHECK_FALSE(snapshot.root.derived.subtreeAllocatedSize);
	CHECK(snapshot.root.derived.knownSubtreeAllocatedSizeLowerBound == 100);
	const SnapshotEntry& partial = snapshot.root.children.at(nativeName("partial"));
	CHECK_FALSE(partial.derived.subtreeAllocatedSize);
	CHECK(partial.derived.knownSubtreeAllocatedSizeLowerBound == 100);
	CHECK_FALSE(snapshot.root.children.at(nativeName("unknown")).derived.knownSubtreeAllocatedSizeLowerBound);
}

TEST_CASE("Derived accounting identifies unavailable and inconsistent hard-link facts", "[snapshot][accounting]")
{
	SECTION("Multiple links without identity are uncertain")
	{
		Snapshot snapshot = makeSnapshot();
		snapshot.root.children.try_emplace(nativeName("file"), regularFile(100, 2));
		snapshot.rebuildDerivedData();
		CHECK_FALSE(snapshot.root.children.at(nativeName("file")).derived.localAllocatedSize);
		CHECK_FALSE(snapshot.root.derived.subtreeAllocatedSize);
		CHECK(snapshot.root.derived.knownSubtreeAllocatedSizeLowerBound == 0);
	}

	SECTION("Unobserved aliases are uncertain")
	{
		Snapshot snapshot = makeSnapshot();
		snapshot.root.children.try_emplace(nativeName("file"), regularFile(100, 2, entryIdentity(42, 7)));
		snapshot.rebuildDerivedData();
		REQUIRE(snapshot.hardLinkGroups.size() == 1);
		CHECK(snapshot.hardLinkGroups.front().metadataConsistent);
		CHECK_FALSE(snapshot.hardLinkGroups.front().allAliasesObserved);
		CHECK_FALSE(snapshot.hardLinkGroups.front().accountingExact);
		CHECK_FALSE(snapshot.root.derived.subtreeAllocatedSize);
	}

	SECTION("Conflicting alias metadata is uncertain")
	{
		Snapshot snapshot = makeSnapshot();
		const thin_io::entry_identity identity = entryIdentity(42, 8);
		snapshot.root.children.try_emplace(nativeName("a"), regularFile(100, 2, identity));
		snapshot.root.children.try_emplace(nativeName("b"), regularFile(200, 2, identity));
		snapshot.rebuildDerivedData();
		REQUIRE(snapshot.hardLinkGroups.size() == 1);
		CHECK_FALSE(snapshot.hardLinkGroups.front().metadataConsistent);
		CHECK_FALSE(snapshot.hardLinkGroups.front().accountingExact);
		CHECK_FALSE(snapshot.root.children.at(nativeName("a")).derived.localAllocatedSize);
		CHECK_FALSE(snapshot.root.children.at(nativeName("b")).derived.localAllocatedSize);
		CHECK_FALSE(snapshot.root.derived.subtreeAllocatedSize);
	}
}

TEST_CASE("Derived accounting rejects allocated-size overflow", "[snapshot][accounting]")
{
	Snapshot snapshot = makeSnapshot();
	snapshot.root.children.try_emplace(nativeName("a"), regularFile(std::numeric_limits<uint64_t>::max()));
	snapshot.root.children.try_emplace(nativeName("b"), regularFile(1));
	snapshot.rebuildDerivedData();
	CHECK(snapshot.root.derived.allocationOverflow);
	CHECK_FALSE(snapshot.root.derived.subtreeAllocatedSize);
	CHECK_FALSE(snapshot.root.derived.knownSubtreeAllocatedSizeLowerBound);
}

TEST_CASE("Derived accounting excludes mount boundaries but includes link entries", "[snapshot][accounting]")
{
	Snapshot snapshot = makeSnapshot();
	SnapshotEntry mount = directory(DirectoryTraversalState::mount_boundary);
	mount.metadata = entryMetadata(500, 1, entryIdentity(99, 1));
	SnapshotEntry link = directory(DirectoryTraversalState::link_boundary);
	link.attributes.is_link = true;
	link.metadata = entryMetadata(20);
	snapshot.root.children.try_emplace(nativeName("link"), std::move(link));
	snapshot.root.children.try_emplace(nativeName("mount"), std::move(mount));

	snapshot.rebuildDerivedData();
	CHECK(snapshot.root.children.at(nativeName("mount")).derived.localAllocatedSize == 0);
	CHECK(snapshot.root.children.at(nativeName("link")).derived.localAllocatedSize == 20);
	CHECK(snapshot.root.derived.subtreeAllocatedSize == 20);
	CHECK(snapshot.root.derived.subtreeCoverageComplete);
}

TEST_CASE("Comparison reports lowest significant positive changes", "[snapshot][comparison]")
{
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("existing"), regularFile(100));
	baseline.root.children.try_emplace(nativeName("deleted"), regularFile(50));

	Snapshot current = makeSnapshot();
	current.root.children.try_emplace(nativeName("existing"), regularFile(150));
	SnapshotEntry addedDirectory = directory();
	addedDirectory.children.try_emplace(nativeName("large"), regularFile(70));
	addedDirectory.children.try_emplace(nativeName("small"), regularFile(40));
	current.root.children.try_emplace(nativeName("added"), std::move(addedDirectory));

	const auto result = comparePrepared(baseline, current, 50);
	REQUIRE(result);
	REQUIRE(result->changes.size() == 2);
	const ComparisonChange* existing = findChange(*result, childPath(baseline.rootPath, "existing"));
	const ComparisonChange* added = findChange(*result, childPath(childPath(baseline.rootPath, "added"), "large"));
	REQUIRE(existing);
	REQUIRE(added);
	CHECK(*existing == expectedChange(childPath(baseline.rootPath, "existing"), 100, 150, thin_io::entry_kind::regular_file, true));
	CHECK(*added == expectedChange(
		childPath(childPath(baseline.rootPath, "added"), "large"), 0, 70, thin_io::entry_kind::regular_file, false));
	CHECK(result->summary.allocatedTreeChange == (MagnitudeChange{ChangeDirection::increase, 110}));

	const auto aboveThreshold = compareSnapshots(baseline, current, 111);
	REQUIRE(aboveThreshold);
	CHECK(aboveThreshold->changes.empty());
	CHECK(aboveThreshold->hasPositiveChangeBelowThreshold);

	const auto withoutThreshold = compareSnapshots(baseline, current, 0);
	REQUIRE(withoutThreshold);
	CHECK_FALSE(withoutThreshold->hasPositiveChangeBelowThreshold);
}

TEST_CASE("Comparison reports an aggregate when significant descendants are absent", "[snapshot][comparison]")
{
	SECTION("New directory")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
		SnapshotEntry addedDirectory = directory();
		addedDirectory.children.try_emplace(nativeName("a"), regularFile(30));
		addedDirectory.children.try_emplace(nativeName("b"), regularFile(30));
		current.root.children.try_emplace(nativeName("added"), std::move(addedDirectory));

		const auto result = comparePrepared(baseline, current, 50);
		REQUIRE(result);
		REQUIRE(result->changes.size() == 1);
		CHECK(result->changes.front() == expectedChange(
			childPath(current.rootPath, "added"), 0, 60, thin_io::entry_kind::directory, false));
	}

	SECTION("Existing directory")
	{
		Snapshot baseline = makeSnapshot();
		SnapshotEntry baselineDirectory = directory();
		baselineDirectory.children.try_emplace(nativeName("a"), regularFile(10));
		baselineDirectory.children.try_emplace(nativeName("b"), regularFile(10));
		baseline.root.children.try_emplace(nativeName("existing"), std::move(baselineDirectory));

		Snapshot current = makeSnapshot();
		SnapshotEntry currentDirectory = directory();
		currentDirectory.children.try_emplace(nativeName("a"), regularFile(30));
		currentDirectory.children.try_emplace(nativeName("b"), regularFile(30));
		current.root.children.try_emplace(nativeName("existing"), std::move(currentDirectory));

		const auto result = comparePrepared(baseline, current, 30);
		REQUIRE(result);
		REQUIRE(result->changes.size() == 1);
		CHECK(result->changes.front() == expectedChange(
			childPath(current.rootPath, "existing"), 20, 60, thin_io::entry_kind::directory, true));
	}
}

TEST_CASE("Comparison keeps positive growth visible when larger deletions increase free space", "[snapshot][comparison][space]")
{
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("deleted"), regularFile(100));

	Snapshot current = makeSnapshot();
	current.root.children.try_emplace(nativeName("accumulated"), regularFile(40));
	current.filesystemSpaceAtStart = thin_io::filesystem_space{1000, 570, 470, 42};
	current.filesystemSpaceAtCompletion = thin_io::filesystem_space{1000, 560, 460, 42};

	const auto result = comparePrepared(baseline, current, 1);
	REQUIRE(result);
	REQUIRE(result->changes.size() == 1);
	CHECK(result->changes.front() == expectedChange(
		childPath(current.rootPath, "accumulated"), 0, 40, thin_io::entry_kind::regular_file, false));
	CHECK(result->summary.freeSpaceChange == (MagnitudeChange{ChangeDirection::increase, 60}));
	CHECK(result->summary.allocatedTreeChange == (MagnitudeChange{ChangeDirection::decrease, 60}));
	CHECK(result->summary.unexplainedConsumptionChange == (MagnitudeChange{}));
	CHECK(result->summary.reconciliation == ReconciliationState::exact);
}

TEST_CASE("Incomplete regions do not hide comparable siblings", "[snapshot][comparison]")
{
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("bad"), directory());
	baseline.root.children.try_emplace(nativeName("good"), regularFile(100));

	Snapshot current = makeSnapshot();
	current.root.children.try_emplace(nativeName("bad"), directory(DirectoryTraversalState::enumeration_failed));
	current.root.children.try_emplace(nativeName("good"), regularFile(200));

	const auto result = comparePrepared(baseline, current, 50);
	REQUIRE(result);
	REQUIRE(result->changes.size() == 1);
	CHECK(result->changes.front() == expectedChange(childPath(current.rootPath, "good"), 100, 200, thin_io::entry_kind::regular_file, true));
	const ComparisonExcludedRegion* bad = findExcludedRegion(*result, childPath(current.rootPath, "bad"));
	REQUIRE(bad);
	CHECK_FALSE(bad->baselineCoverageIncomplete);
	CHECK(bad->currentCoverageIncomplete);
	CHECK_FALSE(result->summary.allocatedTreeChange);
	CHECK(result->summary.reconciliation == ReconciliationState::incomplete);
}

TEST_CASE("Comparison results own source-derived paths", "[snapshot][comparison][lifetime]")
{
	SnapshotComparisonResult comparison;
	NativePath changedPath;
	NativePath excludedPath;
	{
		Snapshot baseline = makeSnapshot();
		baseline.root.children.try_emplace(nativeName("excluded"), directory());
		Snapshot current = makeSnapshot();
		current.root.children.try_emplace(nativeName("changed"), regularFile(100));
		current.root.children.try_emplace(nativeName("excluded"), directory(DirectoryTraversalState::enumeration_failed));
		changedPath = childPath(current.rootPath, "changed");
		excludedPath = childPath(current.rootPath, "excluded");

		auto result = comparePrepared(baseline, current, 1);
		REQUIRE(result);
		comparison = std::move(*result);
	}

	REQUIRE(comparison.changes.size() == 1);
	CHECK(comparison.changes.front() == expectedChange(changedPath, 0, 100, thin_io::entry_kind::regular_file, false));
	REQUIRE(comparison.excludedRegions.size() == 1);
	CHECK(comparison.excludedRegions.front().path == excludedPath);
}

TEST_CASE("Metadata uncertainty excludes only the affected entry", "[snapshot][comparison]")
{
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("unknown"), regularFile(100));
	baseline.root.children.try_emplace(nativeName("good"), regularFile(10));
	Snapshot current = baseline;
	current.root.children.at(nativeName("unknown")).metadata.reset();
	current.root.children.at(nativeName("good")).metadata->allocatedSize = 30;
	current.root.children.at(nativeName("good")).metadata->logicalSize = 30;

	const auto result = comparePrepared(baseline, current, 20);
	REQUIRE(result);
	CHECK(findChange(*result, childPath(current.rootPath, "good")) != nullptr);
	const ComparisonExcludedRegion* unknown = findExcludedRegion(*result, childPath(current.rootPath, "unknown"));
	REQUIRE(unknown);
	CHECK(unknown->currentAccountingUncertain);
	CHECK_FALSE(unknown->currentCoverageIncomplete);
}

TEST_CASE("Common hard-link aliases anchor groups across snapshots", "[snapshot][comparison][hard-links]")
{
	const thin_io::entry_identity identity = entryIdentity(42, 4);
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("b"), regularFile(100, 1, identity));

	Snapshot current = makeSnapshot();
	current.root.children.try_emplace(nativeName("a"), regularFile(100, 2, identity));
	current.root.children.try_emplace(nativeName("b"), regularFile(100, 2, identity));

	const auto aliasAdded = comparePrepared(baseline, current, 1);
	REQUIRE(aliasAdded);
	CHECK(aliasAdded->changes.empty());
	CHECK(aliasAdded->summary.allocatedTreeChange == (MagnitudeChange{}));

	current.root.children.at(nativeName("a")).metadata->allocatedSize = 150;
	current.root.children.at(nativeName("a")).metadata->logicalSize = 150;
	current.root.children.at(nativeName("b")).metadata->allocatedSize = 150;
	current.root.children.at(nativeName("b")).metadata->logicalSize = 150;
	const auto allocationGrew = comparePrepared(baseline, current, 1);
	REQUIRE(allocationGrew);
	REQUIRE(allocationGrew->changes.size() == 1);
	CHECK(allocationGrew->changes.front() == expectedChange(childPath(current.rootPath, "b"), 100, 150, thin_io::entry_kind::regular_file, true));

	Snapshot twoAliases = makeSnapshot();
	twoAliases.root.children.try_emplace(nativeName("a"), regularFile(100, 2, identity));
	twoAliases.root.children.try_emplace(nativeName("b"), regularFile(100, 2, identity));
	Snapshot aliasRemoved = makeSnapshot();
	aliasRemoved.root.children.try_emplace(nativeName("b"), regularFile(100, 1, identity));
	const auto removal = comparePrepared(twoAliases, aliasRemoved, 1);
	REQUIRE(removal);
	CHECK(removal->changes.empty());
	CHECK(removal->summary.allocatedTreeChange == (MagnitudeChange{}));
}

TEST_CASE("Equal identities at disjoint paths are not treated as moves", "[snapshot][comparison][hard-links]")
{
	const thin_io::entry_identity identity = entryIdentity(42, 5);
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("old"), regularFile(100, 1, identity));
	Snapshot current = makeSnapshot();
	current.root.children.try_emplace(nativeName("new"), regularFile(100, 1, identity));

	const auto result = comparePrepared(baseline, current, 1);
	REQUIRE(result);
	REQUIRE(result->changes.size() == 1);
	CHECK(result->changes.front() == expectedChange(childPath(current.rootPath, "new"), 0, 100, thin_io::entry_kind::regular_file, false));
	CHECK(result->summary.allocatedTreeChange == (MagnitudeChange{}));
}

TEST_CASE("Root eligibility uses paths and available identities", "[snapshot][comparison][identity]")
{
	SECTION("Different paths are rejected")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
#ifdef _WIN32
		current.rootPath = R"(C:\other)";
#else
		current.rootPath = "/other";
#endif
		const auto result = comparePrepared(baseline, current, 1);
		REQUIRE_FALSE(result);
		CHECK(result.error() == SnapshotComparisonError::different_root_paths);
	}

	SECTION("Filesystem replacement is rejected")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
		current.root.metadata->identity = entryIdentity(99, 1);
		current.filesystemSpaceAtStart->identity = 99;
		current.filesystemSpaceAtCompletion->identity = 99;
		const auto result = comparePrepared(baseline, current, 1);
		REQUIRE_FALSE(result);
		CHECK(result.error() == SnapshotComparisonError::filesystem_identity_mismatch);
	}

	SECTION("Root replacement is rejected")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
		current.root.metadata->identity = entryIdentity(42, 2);
		const auto result = comparePrepared(baseline, current, 1);
		REQUIRE_FALSE(result);
		CHECK(result.error() == SnapshotComparisonError::root_identity_mismatch);
	}

	SECTION("Unavailable identities permit path-only comparison with warnings")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
		baseline.root.metadata->identity.reset();
		current.root.metadata->identity.reset();
		baseline.filesystemSpaceAtStart->identity.reset();
		baseline.filesystemSpaceAtCompletion->identity.reset();
		current.filesystemSpaceAtStart->identity.reset();
		current.filesystemSpaceAtCompletion->identity.reset();
		const auto result = comparePrepared(baseline, current, 1);
		REQUIRE(result);
		CHECK(result->warnings == (std::vector{
			SnapshotComparisonWarning::filesystem_identity_unavailable,
			SnapshotComparisonWarning::root_identity_unavailable}));
	}

	SECTION("Link roots are rejected")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
		baseline.root.attributes.is_link = true;
		const auto result = comparePrepared(baseline, current, 1);
		REQUIRE_FALSE(result);
		CHECK(result.error() == SnapshotComparisonError::invalid_baseline_root);
	}
}

TEST_CASE("Comparison reconciles free space with exact net allocation", "[snapshot][comparison][space]")
{
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("file"), regularFile(100));
	Snapshot current = makeSnapshot();
	current.root.children.try_emplace(nativeName("file"), regularFile(122));
	current.filesystemSpaceAtStart->capacity = 1100;
	current.filesystemSpaceAtStart->free = 475;
	current.filesystemSpaceAtStart->available = 385;
	current.filesystemSpaceAtCompletion = thin_io::filesystem_space{1100, 470, 380, 42};

	const auto result = comparePrepared(baseline, current, 1);
	REQUIRE(result);
	CHECK(result->summary.freeSpaceChange == (MagnitudeChange{ChangeDirection::decrease, 30}));
	CHECK(result->summary.availableSpaceChange == (MagnitudeChange{ChangeDirection::decrease, 20}));
	CHECK(result->summary.capacityChange == (MagnitudeChange{ChangeDirection::increase, 100}));
	CHECK(result->summary.baselineScanFreeSpaceChange == (MagnitudeChange{ChangeDirection::decrease, 10}));
	CHECK(result->summary.currentScanFreeSpaceChange == (MagnitudeChange{ChangeDirection::decrease, 5}));
	CHECK(result->summary.allocatedTreeChange == (MagnitudeChange{ChangeDirection::increase, 22}));
	CHECK(result->summary.unexplainedConsumptionChange == (MagnitudeChange{ChangeDirection::increase, 8}));
	CHECK(result->summary.reconciliation == ReconciliationState::exact);
}

TEST_CASE("Comparison keeps independent filesystem-space signals separate", "[snapshot][comparison][space]")
{
	SECTION("Available-space-only changes do not affect reconciliation")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
		current.filesystemSpaceAtCompletion->available = 350;
		const auto result = comparePrepared(baseline, current, 1);
		REQUIRE(result);
		CHECK(result->summary.freeSpaceChange == (MagnitudeChange{}));
		CHECK(result->summary.availableSpaceChange == (MagnitudeChange{ChangeDirection::decrease, 50}));
		CHECK(result->summary.allocatedTreeChange == (MagnitudeChange{}));
		CHECK(result->summary.unexplainedConsumptionChange == (MagnitudeChange{}));
	}

	SECTION("Unexplained free-space gain retains its direction")
	{
		Snapshot baseline = makeSnapshot();
		Snapshot current = makeSnapshot();
		current.filesystemSpaceAtCompletion->free = 530;
		const auto result = comparePrepared(baseline, current, 1);
		REQUIRE(result);
		CHECK(result->summary.unexplainedConsumptionChange == (MagnitudeChange{ChangeDirection::decrease, 30}));
		CHECK(result->summary.reconciliation == ReconciliationState::exact);
	}
}

TEST_CASE("Reconciliation detects magnitude overflow", "[snapshot][comparison][space]")
{
	constexpr uint64_t Maximum = std::numeric_limits<uint64_t>::max();
	Snapshot baseline = makeSnapshot();
	baseline.root.children.try_emplace(nativeName("file"), regularFile(Maximum));
	baseline.filesystemSpaceAtCompletion->capacity = Maximum;
	baseline.filesystemSpaceAtCompletion->free = Maximum;
	baseline.filesystemSpaceAtCompletion->available = 0;
	Snapshot current = makeSnapshot();
	current.filesystemSpaceAtCompletion->free = 0;
	current.filesystemSpaceAtCompletion->available = 0;

	const auto result = comparePrepared(baseline, current, 1);
	REQUIRE(result);
	CHECK(result->summary.allocatedTreeChange == (MagnitudeChange{ChangeDirection::decrease, Maximum}));
	CHECK(result->summary.reconciliation == ReconciliationState::overflow);
	CHECK_FALSE(result->summary.unexplainedConsumptionChange);
}
