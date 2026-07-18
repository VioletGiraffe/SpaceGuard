#include "3rdparty/catch2/catch.hpp"

#include "snapshot_scanner.h"
#include "threading/cworkerthread.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace SpaceGuard;

namespace {

NativePath nativePath(const char* path)
{
#ifdef _WIN32
	return QString::fromUtf8(path);
#else
	return QByteArray{path};
#endif
}

NativeName nativeName(const char* name)
{
	return nativePath(name);
}

thin_io::native_string thinIoName(const char* name)
{
#ifdef _WIN32
	return QString::fromUtf8(name).toStdWString();
#else
	return name;
#endif
}

NativePath rootPath()
{
#ifdef _WIN32
	return R"(C:\scan)";
#else
	return "/scan";
#endif
}

thin_io::entry_identity identity(const uint64_t filesystem, const uint8_t seed)
{
	thin_io::entry_identity result;
	result.filesystem = filesystem;
	for (size_t i = 0; i < result.entry.size(); ++i)
		result.entry[i] = static_cast<uint8_t>(seed + i);
	return result;
}

thin_io::entry_attributes attributes(const thin_io::entry_kind kind, const bool isLink = false)
{
	thin_io::entry_attributes result;
	result.kind = kind;
	result.is_link = isLink;
#ifdef _WIN32
	result.reparse_tag = isLink ? 0xA000000C : 0;
#endif
	return result;
}

thin_io::entry_metadata metadata(const thin_io::entry_kind kind, const uint64_t filesystem,
	const uint8_t seed, const uint64_t allocatedSize = 0, const uint64_t hardLinkCount = 1, const bool isLink = false)
{
	return {attributes(kind, isLink), allocatedSize, allocatedSize, hardLinkCount, identity(filesystem, seed)};
}

thin_io::directory_entry listed(const char* name, const thin_io::entry_kind kind, const bool isLink = false)
{
	return {thinIoName(name), attributes(kind, isLink), {}};
}

template <class Value>
thin_io::filesystem_result<Value> error(const thin_io::filesystem_error_code code)
{
	return std::unexpected{thin_io::filesystem_error{code}};
}

enum class FakeOperation {
	list_directory,
	entry_metadata,
	filesystem_space
};

class FakeFilesystem final : public FilesystemAccess
{
public:
	std::map<NativePath, thin_io::filesystem_result<std::vector<thin_io::directory_entry>>> directories;
	std::map<NativePath, thin_io::filesystem_result<thin_io::entry_metadata>> metadataByPath;
	std::vector<thin_io::filesystem_result<thin_io::filesystem_space>> spaceResults;
	std::function<void(FakeOperation, const NativePath&)> afterOperation;
	std::vector<NativePath> listedPaths;
	std::vector<NativePath> metadataPaths;

	thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectory(const NativePath& path) override
	{
		{
			std::lock_guard lock{historyMutex};
			listedPaths.push_back(path);
		}
		const auto result = directories.at(path);
		if (afterOperation)
			afterOperation(FakeOperation::list_directory, path);
		return result;
	}

	thin_io::filesystem_result<thin_io::entry_metadata> getEntryMetadata(
		const NativePath& path, const thin_io::link_behavior linkBehavior) override
	{
		if (linkBehavior != thin_io::link_behavior::do_not_follow)
			throw std::logic_error{"Scanner followed a link"};
		{
			std::lock_guard lock{historyMutex};
			metadataPaths.push_back(path);
		}
		const auto result = metadataByPath.at(path);
		if (afterOperation)
			afterOperation(FakeOperation::entry_metadata, path);
		return result;
	}

	thin_io::filesystem_result<thin_io::filesystem_space> getFilesystemSpace(const NativePath& path) override
	{
		REQUIRE(spaceResultIndex < spaceResults.size());
		const auto result = spaceResults[spaceResultIndex++];
		if (afterOperation)
			afterOperation(FakeOperation::filesystem_space, path);
		return result;
	}

private:
	size_t spaceResultIndex = 0;
	std::mutex historyMutex;
};

void configureRoot(FakeFilesystem& filesystem, std::vector<thin_io::directory_entry> entries = {})
{
	filesystem.metadataByPath.emplace(rootPath(), metadata(thin_io::entry_kind::directory, 7, 1, 4096));
	filesystem.directories.emplace(rootPath(), std::move(entries));
	filesystem.spaceResults = {
		thin_io::filesystem_space{100000, 50000, 45000, 7},
		thin_io::filesystem_space{100000, 49000, 44000, 7}
	};
}

void configureParallelTree(FakeFilesystem& filesystem)
{
	configureRoot(filesystem, {
		listed("directory-a", thin_io::entry_kind::directory),
		listed("directory-b", thin_io::entry_kind::directory),
		listed("directory-c", thin_io::entry_kind::directory),
		listed("missing-metadata", thin_io::entry_kind::regular_file)
	});
	const std::array directories{
		std::pair{"directory-a", uint8_t{2}}, std::pair{"directory-b", uint8_t{3}}, std::pair{"directory-c", uint8_t{4}}
	};
	for (const auto& [name, seed] : directories)
	{
		const NativePath path = appendNativeName(rootPath(), nativeName(name));
		filesystem.metadataByPath.emplace(path, metadata(thin_io::entry_kind::directory, 7, seed, 4096));
		filesystem.directories.emplace(path, std::vector{listed("file", thin_io::entry_kind::regular_file)});
		filesystem.metadataByPath.emplace(appendNativeName(path, nativeName("file")),
			metadata(thin_io::entry_kind::regular_file, 7, static_cast<uint8_t>(seed + 10), seed * 10));
	}
	filesystem.directories[appendNativeName(rootPath(), nativeName("directory-c"))]
		= error<std::vector<thin_io::directory_entry>>(21);
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("missing-metadata")), error<thin_io::entry_metadata>(20));
}

Snapshot completedSnapshot(SnapshotScanResult result)
{
	auto* snapshot = std::get_if<Snapshot>(&result);
	REQUIRE(snapshot);
	return std::move(*snapshot);
}

SnapshotScanFailure failedScan(const SnapshotScanResult& result)
{
	const auto* failure = std::get_if<SnapshotScanFailure>(&result);
	REQUIRE(failure);
	return *failure;
}

std::filesystem::path filesystemPath(const NativePath& path)
{
#ifdef _WIN32
	return std::filesystem::path{reinterpret_cast<const wchar_t*>(path.utf16())};
#else
	return std::filesystem::path{path.constData()};
#endif
}

} // namespace

TEST_CASE("Snapshot scanner reports root-fatal failures", "[snapshot][scanner]")
{
	std::atomic_bool canceled = false;

	SECTION("invalid root")
	{
		FakeFilesystem filesystem;
		CHECK(failedScan(scanSnapshot(nativePath("relative"), filesystem, canceled)).code == SnapshotScanFailureCode::invalid_root);
	}

	SECTION("root metadata failure")
	{
		FakeFilesystem filesystem;
		filesystem.metadataByPath.emplace(rootPath(), error<thin_io::entry_metadata>(5));
		const auto& failure = failedScan(scanSnapshot(rootPath(), filesystem, canceled));
		CHECK(failure.code == SnapshotScanFailureCode::root_metadata_unavailable);
		CHECK(failure.nativeErrorCode == 5);
	}

	SECTION("root must be a real directory")
	{
		FakeFilesystem filesystem;
		filesystem.metadataByPath.emplace(rootPath(), metadata(thin_io::entry_kind::regular_file, 7, 1));
		CHECK(failedScan(scanSnapshot(rootPath(), filesystem, canceled)).code == SnapshotScanFailureCode::root_not_directory);

		FakeFilesystem linkFilesystem;
		linkFilesystem.metadataByPath.emplace(rootPath(), metadata(thin_io::entry_kind::directory, 7, 1, 0, 1, true));
		CHECK(failedScan(scanSnapshot(rootPath(), linkFilesystem, canceled)).code == SnapshotScanFailureCode::root_is_link);
	}

	SECTION("initial filesystem observation is required and must identify the root filesystem")
	{
		FakeFilesystem filesystem;
		configureRoot(filesystem);
		filesystem.spaceResults = {error<thin_io::filesystem_space>(6)};
		CHECK(failedScan(scanSnapshot(rootPath(), filesystem, canceled)).code
			== SnapshotScanFailureCode::filesystem_space_at_start_unavailable);

		FakeFilesystem mismatch;
		configureRoot(mismatch);
		mismatch.spaceResults.front()->identity = 8;
		CHECK(failedScan(scanSnapshot(rootPath(), mismatch, canceled)).code
			== SnapshotScanFailureCode::root_filesystem_identity_mismatch);
	}

	SECTION("root enumeration failure")
	{
		FakeFilesystem filesystem;
		configureRoot(filesystem);
		filesystem.directories[rootPath()] = error<std::vector<thin_io::directory_entry>>(7);
		const auto& failure = failedScan(scanSnapshot(rootPath(), filesystem, canceled));
		CHECK(failure.code == SnapshotScanFailureCode::root_enumeration_unavailable);
		CHECK(failure.nativeErrorCode == 7);
	}

	SECTION("completion observation must still identify the root filesystem")
	{
		FakeFilesystem filesystem;
		configureRoot(filesystem);
		filesystem.spaceResults.back()->identity = 8;
		CHECK(failedScan(scanSnapshot(rootPath(), filesystem, canceled)).code
			== SnapshotScanFailureCode::root_filesystem_identity_changed);
	}
}

TEST_CASE("Snapshot scanner completes an empty root and tolerates a missing completion-space observation", "[snapshot][scanner]")
{
	FakeFilesystem filesystem;
	configureRoot(filesystem);
	filesystem.spaceResults.back() = error<thin_io::filesystem_space>(8);
	std::atomic_bool canceled = false;

	const Snapshot snapshot = completedSnapshot(scanSnapshot(rootPath(), filesystem, canceled));
	CHECK(snapshot.root.traversalState == DirectoryTraversalState::completed);
	CHECK(snapshot.root.children.empty());
	CHECK(snapshot.filesystemSpaceAtStart.has_value());
	CHECK_FALSE(snapshot.filesystemSpaceAtCompletion.has_value());
	REQUIRE(snapshot.diagnostics.size() == 1);
	CHECK(snapshot.diagnostics.front().operation == SnapshotOperation::filesystem_space_at_completion);
	CHECK(snapshot.derivedDataAvailable);
}

TEST_CASE("Snapshot scanner preserves every discovered kind and traverses only ordinary directories", "[snapshot][scanner]")
{
	FakeFilesystem filesystem;
	configureRoot(filesystem, {
		listed("file", thin_io::entry_kind::regular_file),
		listed("directory", thin_io::entry_kind::directory),
		listed("link", thin_io::entry_kind::directory, true),
		listed("other", thin_io::entry_kind::other),
		listed("unknown", thin_io::entry_kind::unknown)
	});
	const NativePath directoryPath = appendNativeName(rootPath(), nativeName("directory"));
	const NativePath nestedPath = appendNativeName(directoryPath, nativeName("nested"));
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("file")), metadata(thin_io::entry_kind::regular_file, 7, 2, 10));
	filesystem.metadataByPath.emplace(directoryPath, metadata(thin_io::entry_kind::directory, 7, 3, 4096));
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("link")), metadata(thin_io::entry_kind::directory, 7, 4, 0, 1, true));
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("other")), metadata(thin_io::entry_kind::other, 7, 5, 2));
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("unknown")), metadata(thin_io::entry_kind::unknown, 7, 6));
	filesystem.directories.emplace(directoryPath, std::vector{listed("nested", thin_io::entry_kind::regular_file)});
	filesystem.metadataByPath.emplace(nestedPath, metadata(thin_io::entry_kind::regular_file, 7, 7, 20));
	std::atomic_bool canceled = false;

	const Snapshot snapshot = completedSnapshot(scanSnapshot(rootPath(), filesystem, canceled));
	CHECK(snapshot.root.children.at(nativeName("directory")).traversalState == DirectoryTraversalState::completed);
	CHECK(snapshot.root.children.at(nativeName("directory")).children.contains(nativeName("nested")));
	CHECK(snapshot.root.children.at(nativeName("link")).traversalState == DirectoryTraversalState::link_boundary);
	CHECK(snapshot.root.children.at(nativeName("file")).traversalState == DirectoryTraversalState::not_directory);
	CHECK(snapshot.root.children.at(nativeName("other")).metadata.has_value());
	CHECK(snapshot.root.children.at(nativeName("unknown")).metadata.has_value());
	CHECK(std::ranges::find(filesystem.listedPaths, appendNativeName(rootPath(), nativeName("link"))) == filesystem.listedPaths.end());
}

TEST_CASE("Snapshot scanner contains descendant failures and filesystem boundaries", "[snapshot][scanner]")
{
	FakeFilesystem filesystem;
	configureRoot(filesystem, {
		listed("failed-list", thin_io::entry_kind::directory),
		listed("failed-metadata", thin_io::entry_kind::directory),
		listed("boundary", thin_io::entry_kind::directory),
		listed("sibling", thin_io::entry_kind::regular_file)
	});
	const NativePath failedListPath = appendNativeName(rootPath(), nativeName("failed-list"));
	const NativePath failedMetadataPath = appendNativeName(rootPath(), nativeName("failed-metadata"));
	const NativePath boundaryPath = appendNativeName(rootPath(), nativeName("boundary"));
	filesystem.metadataByPath.emplace(failedListPath, metadata(thin_io::entry_kind::directory, 7, 2));
	filesystem.directories.emplace(failedListPath, error<std::vector<thin_io::directory_entry>>(10));
	filesystem.metadataByPath.emplace(failedMetadataPath, error<thin_io::entry_metadata>(11));
	filesystem.metadataByPath.emplace(boundaryPath, metadata(thin_io::entry_kind::directory, 8, 3));
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("sibling")), metadata(thin_io::entry_kind::regular_file, 7, 4, 32));
	std::atomic_bool canceled = false;

	const Snapshot snapshot = completedSnapshot(scanSnapshot(rootPath(), filesystem, canceled));
	CHECK(snapshot.root.children.at(nativeName("failed-list")).traversalState == DirectoryTraversalState::enumeration_failed);
	CHECK(snapshot.root.children.at(nativeName("failed-metadata")).traversalState == DirectoryTraversalState::metadata_unavailable);
	CHECK(snapshot.root.children.at(nativeName("boundary")).traversalState == DirectoryTraversalState::filesystem_boundary);
	CHECK(snapshot.root.children.at(nativeName("sibling")).metadata.has_value());
	CHECK(snapshot.diagnostics.size() == 2);
	CHECK_FALSE(snapshot.root.derived.subtreeCoverageComplete);
}

TEST_CASE("Snapshot scanner marks entries replaced between listing and metadata", "[snapshot][scanner]")
{
	FakeFilesystem filesystem;
	configureRoot(filesystem, {
		listed("vanished", thin_io::entry_kind::regular_file),
		listed("replaced", thin_io::entry_kind::directory)
	});
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("vanished")), error<thin_io::entry_metadata>(12));
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("replaced")), metadata(thin_io::entry_kind::regular_file, 7, 2));
	std::atomic_bool canceled = false;

	const Snapshot snapshot = completedSnapshot(scanSnapshot(rootPath(), filesystem, canceled));
	const SnapshotEntry& vanished = snapshot.root.children.at(nativeName("vanished"));
	const SnapshotEntry& replaced = snapshot.root.children.at(nativeName("replaced"));
	CHECK_FALSE(vanished.metadata.has_value());
	CHECK(replaced.traversalState == DirectoryTraversalState::metadata_unavailable);
	CHECK_FALSE(replaced.metadata.has_value());
	REQUIRE(snapshot.diagnostics.size() == 2);
	CHECK(snapshot.diagnostics[0].operation == SnapshotOperation::entry_changed_during_scan);
	CHECK_FALSE(snapshot.diagnostics[0].nativeErrorCode.has_value());
	CHECK(snapshot.diagnostics[1].operation == SnapshotOperation::entry_metadata);
	CHECK(snapshot.diagnostics[1].nativeErrorCode == 12);
}

TEST_CASE("Snapshot scanner cancellation never returns a partial snapshot", "[snapshot][scanner]")
{
	SECTION("before start")
	{
		FakeFilesystem filesystem;
		std::atomic_bool canceled = true;
		CHECK(std::holds_alternative<SnapshotScanCanceled>(scanSnapshot(rootPath(), filesystem, canceled)));
	}

	SECTION("between directories")
	{
		FakeFilesystem filesystem;
		configureRoot(filesystem, {listed("directory", thin_io::entry_kind::directory)});
		const NativePath directoryPath = appendNativeName(rootPath(), nativeName("directory"));
		filesystem.metadataByPath.emplace(directoryPath, metadata(thin_io::entry_kind::directory, 7, 2));
		filesystem.directories.emplace(directoryPath, std::vector<thin_io::directory_entry>{});
		std::atomic_bool canceled = false;
		filesystem.afterOperation = [&canceled, &directoryPath](const FakeOperation operation, const NativePath& path) {
			if (operation == FakeOperation::entry_metadata && path == directoryPath)
				canceled = true;
		};
		CHECK(std::holds_alternative<SnapshotScanCanceled>(scanSnapshot(rootPath(), filesystem, canceled)));
		CHECK(std::ranges::find(filesystem.listedPaths, directoryPath) == filesystem.listedPaths.end());
	}

	SECTION("inside a wide entry loop")
	{
		FakeFilesystem filesystem;
		std::vector<thin_io::directory_entry> entries;
		for (int i = 0; i < 20; ++i)
		{
			const std::string name = "file-" + std::to_string(i);
			entries.push_back(listed(name.c_str(), thin_io::entry_kind::regular_file));
			filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName(name.c_str())),
				metadata(thin_io::entry_kind::regular_file, 7, static_cast<uint8_t>(i + 2), 1));
		}
		configureRoot(filesystem, std::move(entries));
		std::atomic_bool canceled = false;
		int childMetadataCalls = 0;
		filesystem.afterOperation = [&canceled, &childMetadataCalls](const FakeOperation operation, const NativePath& path) {
			if (operation == FakeOperation::entry_metadata && path != rootPath() && ++childMetadataCalls == 5)
				canceled = true;
		};
		CHECK(std::holds_alternative<SnapshotScanCanceled>(scanSnapshot(rootPath(), filesystem, canceled)));
		CHECK(childMetadataCalls == 5);
	}
}

TEST_CASE("Snapshot scanner output is independent of enumeration order", "[snapshot][scanner]")
{
	auto configure = [](FakeFilesystem& filesystem, const bool reverse) {
		std::vector entries{
			listed("b", thin_io::entry_kind::regular_file),
			listed("a", thin_io::entry_kind::regular_file)
		};
		if (reverse)
			std::ranges::reverse(entries);
		configureRoot(filesystem, std::move(entries));
		filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("a")), metadata(thin_io::entry_kind::regular_file, 7, 2, 8));
		filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("b")), metadata(thin_io::entry_kind::regular_file, 7, 3, 16));
	};
	FakeFilesystem firstFilesystem;
	FakeFilesystem secondFilesystem;
	configure(firstFilesystem, false);
	configure(secondFilesystem, true);
	std::atomic_bool canceled = false;
	Snapshot first = completedSnapshot(scanSnapshot(rootPath(), firstFilesystem, canceled));
	Snapshot second = completedSnapshot(scanSnapshot(rootPath(), secondFilesystem, canceled));
	second.scanStartedAtUtc = first.scanStartedAtUtc;
	second.scanCompletedAtUtc = first.scanCompletedAtUtc;
	CHECK(first == second);
	CHECK(first.root.derived.subtreeAllocatedSize == second.root.derived.subtreeAllocatedSize);
}

TEST_CASE("Parallel snapshot scanning waits for discovered work and matches single-thread output", "[snapshot][scanner][parallel]")
{
	FakeFilesystem singleThreadFilesystem;
	FakeFilesystem parallelFilesystem;
	configureParallelTree(singleThreadFilesystem);
	configureParallelTree(parallelFilesystem);
	std::atomic_bool canceled = false;
	Snapshot singleThreadSnapshot = completedSnapshot(scanSnapshot(rootPath(), singleThreadFilesystem, canceled));

	std::mutex concurrencyMutex;
	std::condition_variable concurrentDirectoryEntered;
	int activeDirectoryCalls = 0;
	int maximumConcurrentDirectoryCalls = 0;
	bool releaseDirectoryCalls = false;
	parallelFilesystem.afterOperation = [&](const FakeOperation operation, const NativePath& path) {
		if (operation != FakeOperation::list_directory || path == rootPath())
			return;
		std::unique_lock lock{concurrencyMutex};
		++activeDirectoryCalls;
		maximumConcurrentDirectoryCalls = std::max(maximumConcurrentDirectoryCalls, activeDirectoryCalls);
		if (activeDirectoryCalls >= 2)
		{
			releaseDirectoryCalls = true;
			concurrentDirectoryEntered.notify_all();
		}
		else
		{
			concurrentDirectoryEntered.wait_for(lock, std::chrono::seconds{2}, [&releaseDirectoryCalls] { return releaseDirectoryCalls; });
		}
		--activeDirectoryCalls;
	};

	CWorkerThreadPool workerPool{3, "SpaceGuard scanner test"};
	std::vector<SnapshotScanProgress> progress;
	Snapshot parallelSnapshot = completedSnapshot(scanSnapshot(rootPath(), parallelFilesystem, canceled,
		[&progress](const SnapshotScanProgress& value) { progress.push_back(value); }, &workerPool));
	CHECK(maximumConcurrentDirectoryCalls >= 2);
	REQUIRE_FALSE(progress.empty());
	for (size_t i = 1; i < progress.size(); ++i)
	{
		CHECK(progress[i].directoriesCompleted >= progress[i - 1].directoriesCompleted);
		CHECK(progress[i].entriesDiscovered >= progress[i - 1].entriesDiscovered);
		CHECK(progress[i].issues >= progress[i - 1].issues);
	}
	CHECK(progress.back() == (SnapshotScanProgress{4, 6, 2}));

	parallelSnapshot.scanStartedAtUtc = singleThreadSnapshot.scanStartedAtUtc;
	parallelSnapshot.scanCompletedAtUtc = singleThreadSnapshot.scanCompletedAtUtc;
	CHECK(parallelSnapshot == singleThreadSnapshot);
	CHECK(parallelSnapshot.root.derived.subtreeAllocatedSize == singleThreadSnapshot.root.derived.subtreeAllocatedSize);
}

TEST_CASE("Parallel snapshot scanning cancels without stranding idle workers", "[snapshot][scanner][parallel]")
{
	FakeFilesystem filesystem;
	configureParallelTree(filesystem);
	const NativePath blockedPath = appendNativeName(rootPath(), nativeName("directory-a"));
	std::mutex blockMutex;
	std::condition_variable blockCondition;
	bool blocked = false;
	bool released = false;
	filesystem.afterOperation = [&](const FakeOperation operation, const NativePath& path) {
		if (operation != FakeOperation::list_directory || path != blockedPath)
			return;
		std::unique_lock lock{blockMutex};
		blocked = true;
		blockCondition.notify_all();
		blockCondition.wait(lock, [&released] { return released; });
	};

	std::atomic_bool canceled = false;
	CWorkerThreadPool workerPool{3, "SpaceGuard scanner cancellation test"};
	auto scan = std::async(std::launch::async, [&] { return scanSnapshot(rootPath(), filesystem, canceled, {}, &workerPool); });
	bool reachedBlock = false;
	{
		std::unique_lock lock{blockMutex};
		reachedBlock = blockCondition.wait_for(lock, std::chrono::seconds{2}, [&blocked] { return blocked; });
		canceled.store(true, std::memory_order_relaxed);
		released = true;
		blockCondition.notify_all();
	}
	REQUIRE(reachedBlock);
	REQUIRE(scan.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	CHECK(std::holds_alternative<SnapshotScanCanceled>(scan.get()));
}

TEST_CASE("Parallel snapshot scanning cancels before newly discovered directories are published", "[snapshot][scanner][parallel]")
{
	FakeFilesystem filesystem;
	configureParallelTree(filesystem);
	std::atomic_bool canceled = false;
	CWorkerThreadPool workerPool{3, "SpaceGuard scanner publication cancellation test"};

	const SnapshotScanResult result = scanSnapshot(rootPath(), filesystem, canceled,
		[&canceled](const SnapshotScanProgress& progress) {
			if (progress.entriesDiscovered > 0)
				canceled.store(true, std::memory_order_relaxed);
		}, &workerPool);
	CHECK(std::holds_alternative<SnapshotScanCanceled>(result));
	CHECK(filesystem.listedPaths.size() == 1);
}

TEST_CASE("Parallel snapshot scanning contains worker exceptions without deadlock", "[snapshot][scanner][parallel]")
{
	FakeFilesystem filesystem;
	configureParallelTree(filesystem);
	const NativePath throwingPath = appendNativeName(rootPath(), nativeName("directory-b"));
	filesystem.afterOperation = [&throwingPath](const FakeOperation operation, const NativePath& path) {
		if (operation == FakeOperation::list_directory && path == throwingPath)
			throw std::runtime_error{"Injected directory failure"};
	};

	std::atomic_bool canceled = false;
	CWorkerThreadPool workerPool{3, "SpaceGuard scanner exception test"};
	auto scan = std::async(std::launch::async, [&] { return scanSnapshot(rootPath(), filesystem, canceled, {}, &workerPool); });
	REQUIRE(scan.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
	const SnapshotScanResult result = scan.get();
	const auto* failure = std::get_if<SnapshotScanFailure>(&result);
	REQUIRE(failure);
	CHECK(failure->code == SnapshotScanFailureCode::unexpected_error);
}

TEST_CASE("Parallel snapshot output is deterministic across enumeration and scheduling order", "[snapshot][scanner][parallel]")
{
	FakeFilesystem referenceFilesystem;
	configureParallelTree(referenceFilesystem);
	std::atomic_bool canceled = false;
	Snapshot reference = completedSnapshot(scanSnapshot(rootPath(), referenceFilesystem, canceled));
	CWorkerThreadPool workerPool{3, "SpaceGuard scanner determinism test"};

	for (int run = 0; run < 8; ++run)
	{
		FakeFilesystem filesystem;
		configureParallelTree(filesystem);
		std::mt19937 random{static_cast<uint32_t>(run + 1)};
		auto& rootEntries = *filesystem.directories.at(rootPath());
		std::shuffle(rootEntries.begin(), rootEntries.end(), random);
		filesystem.afterOperation = [run](const FakeOperation operation, const NativePath& path) {
			const int yields = (run + static_cast<int>(path.size()) + static_cast<int>(operation)) % 4;
			for (int i = 0; i < yields; ++i)
				std::this_thread::yield();
		};

		Snapshot snapshot = completedSnapshot(scanSnapshot(rootPath(), filesystem, canceled, {}, &workerPool));
		snapshot.scanStartedAtUtc = reference.scanStartedAtUtc;
		snapshot.scanCompletedAtUtc = reference.scanCompletedAtUtc;
		CHECK(snapshot == reference);
		CHECK(snapshot.root.derived.subtreeAllocatedSize == reference.root.derived.subtreeAllocatedSize);
	}
}

TEST_CASE("Snapshot scanner progress is monotonic", "[snapshot][scanner]")
{
	FakeFilesystem filesystem;
	configureRoot(filesystem, {
		listed("good", thin_io::entry_kind::regular_file),
		listed("failed", thin_io::entry_kind::regular_file)
	});
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("good")), metadata(thin_io::entry_kind::regular_file, 7, 2, 8));
	filesystem.metadataByPath.emplace(appendNativeName(rootPath(), nativeName("failed")), error<thin_io::entry_metadata>(13));
	std::atomic_bool canceled = false;
	std::vector<SnapshotScanProgress> progress;

	completedSnapshot(scanSnapshot(rootPath(), filesystem, canceled,
		[&progress](const SnapshotScanProgress& value) { progress.push_back(value); }));
	REQUIRE_FALSE(progress.empty());
	for (size_t i = 1; i < progress.size(); ++i)
	{
		CHECK(progress[i].directoriesCompleted >= progress[i - 1].directoriesCompleted);
		CHECK(progress[i].entriesDiscovered >= progress[i - 1].entriesDiscovered);
		CHECK(progress[i].issues >= progress[i - 1].issues);
	}
	CHECK(progress.back() == (SnapshotScanProgress{1, 2, 1}));
}

TEST_CASE("Snapshot scanner handles native real-filesystem names, nesting, hard links, and links", "[snapshot][scanner][integration]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	QDir root{directory.path()};
	REQUIRE(root.mkdir(QString::fromUtf8("nested-\xD0\x96")));
	const QString originalPath = root.filePath("nested-\xD0\x96/original.bin");
	QFile original{originalPath};
	REQUIRE(original.open(QIODevice::WriteOnly));
	REQUIRE(original.write("payload") == 7);
	original.close();
	QFile sparse{root.filePath("sparse.bin")};
	REQUIRE(sparse.open(QIODevice::WriteOnly));
	REQUIRE(sparse.seek(1024 * 1024));
	REQUIRE(sparse.write("x") == 1);
	sparse.close();

	const auto nativeRoot = normalizedAbsoluteNativePath(directory.path());
	REQUIRE(nativeRoot);
	std::error_code hardLinkError;
	std::filesystem::create_hard_link(filesystemPath(*normalizedAbsoluteNativePath(originalPath)),
		filesystemPath(*nativeRoot) / "hard-link.bin", hardLinkError);

	std::error_code symbolicLinkError;
	std::filesystem::create_directory_symlink(filesystemPath(*normalizedAbsoluteNativePath(root.filePath("nested-\xD0\x96"))),
		filesystemPath(*nativeRoot) / "directory-link", symbolicLinkError);

	ThinIoFilesystemAccess filesystem;
	std::atomic_bool canceled = false;
	const Snapshot snapshot = completedSnapshot(scanSnapshot(*nativeRoot, filesystem, canceled));
	CHECK(snapshot.root.children.contains(nativeName("nested-\xD0\x96")));
	if (hardLinkError)
		WARN("Hard-link integration check skipped: " << hardLinkError.message());
	else
		CHECK(std::ranges::count_if(snapshot.hardLinkGroups, [](const SnapshotHardLinkGroup& group) {
			return group.reportedLinkCount == 2 && group.aliases.size() == 2;
		}) == 1);
	if (symbolicLinkError)
		WARN("Symbolic-link integration check skipped: " << symbolicLinkError.message());
	else
		CHECK(snapshot.root.children.at(nativeName("directory-link")).attributes.is_link);
	const SnapshotEntry& sparseEntry = snapshot.root.children.at(nativeName("sparse.bin"));
	REQUIRE(sparseEntry.metadata);
	CHECK(sparseEntry.metadata->logicalSize == 1024 * 1024 + 1);
	if (sparseEntry.metadata->allocatedSize >= sparseEntry.metadata->logicalSize)
		WARN("Sparse allocation check skipped because the temporary filesystem allocated the full logical size");
}

#ifndef _WIN32
TEST_CASE("Snapshot scanner preserves non-UTF-8 POSIX names", "[snapshot][scanner][integration]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const auto nativeRoot = normalizedAbsoluteNativePath(directory.path());
	REQUIRE(nativeRoot);
	NativeName rawName{"raw-"};
	rawName.push_back(static_cast<char>(0xFF));
	const NativePath rawPath = appendNativeName(*nativeRoot, rawName);
	std::ofstream file{filesystemPath(rawPath), std::ios::binary};
	if (!file.good())
	{
		WARN("Non-UTF-8 native-name check skipped because the temporary filesystem rejected the name");
		return;
	}
	file << "data";
	file.close();

	ThinIoFilesystemAccess filesystem;
	std::atomic_bool canceled = false;
	const Snapshot snapshot = completedSnapshot(scanSnapshot(*nativeRoot, filesystem, canceled));
	CHECK(snapshot.root.children.contains(rawName));
}
#endif
