#include "3rdparty/catch2/catch.hpp"

#include "snapshot_scan_runner.h"
#include "test_filesystem_access_adapter.h"

#include <assert.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

NativePath rootPath()
{
#ifdef _WIN32
	return R"(C:\runner-scan)";
#else
	return "/runner-scan";
#endif
}

thin_io::native_string nativeName(const std::string& name)
{
#ifdef _WIN32
	return std::wstring{name.begin(), name.end()};
#else
	return name;
#endif
}

thin_io::entry_identity identity(const uint64_t entryNumber)
{
	thin_io::entry_identity result;
	result.filesystem = 7;
	for (size_t i = 0; i < sizeof(entryNumber); ++i)
		result.entry[i] = static_cast<uint8_t>(entryNumber >> (i * 8));
	return result;
}

thin_io::entry_metadata directoryMetadata()
{
	thin_io::entry_metadata result;
	result.attributes.kind = thin_io::entry_kind::directory;
	result.allocated_size = 4096;
	result.hard_link_count = 1;
	result.identity = identity(1);
	return result;
}

thin_io::entry_metadata fileMetadata(const uint64_t entryNumber)
{
	thin_io::entry_metadata result;
	result.attributes.kind = thin_io::entry_kind::regular_file;
	result.logical_size = entryNumber;
	result.allocated_size = entryNumber;
	result.hard_link_count = 1;
	result.identity = identity(entryNumber + 1);
	return result;
}

enum class FilesystemBehavior {
	success,
	recoverable_metadata_failure,
	fatal_root_failure,
	throw_unexpectedly
};

enum class BlockPoint {
	none,
	root_enumeration,
	completion_space
};

class ControlledFilesystem final
{
public:
	ControlledFilesystem(const size_t entryCount = 4,
		const FilesystemBehavior behavior = FilesystemBehavior::success, const BlockPoint blockPoint = BlockPoint::none)
		: m_entryCount{entryCount}, m_behavior{behavior}, m_blockPoint{blockPoint}
	{
	}

	thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectory(const NativePath&)
	{
		if (m_behavior == FilesystemBehavior::throw_unexpectedly)
			throw 42;
		blockIfRequested(BlockPoint::root_enumeration);

		std::vector<thin_io::directory_entry> entries;
		entries.reserve(m_entryCount);
		for (size_t i = 0; i < m_entryCount; ++i)
		{
			thin_io::directory_entry entry;
			entry.name = nativeName("file-" + std::to_string(i));
			entry.attributes.kind = thin_io::entry_kind::regular_file;
			entries.push_back(std::move(entry));
		}
		return entries;
	}

	thin_io::filesystem_result<thin_io::entry_metadata> getEntryMetadata(
		const NativePath& path, const thin_io::link_behavior linkBehavior)
	{
		assert(linkBehavior == thin_io::link_behavior::do_not_follow);
		if (path == rootPath())
		{
			if (m_behavior == FilesystemBehavior::fatal_root_failure)
				return std::unexpected{thin_io::filesystem_error{5}};
			return directoryMetadata();
		}

		const uint64_t entryNumber = m_childMetadataCalls.fetch_add(1, std::memory_order_relaxed);
		if (m_behavior == FilesystemBehavior::recoverable_metadata_failure && entryNumber == 0)
			return std::unexpected{thin_io::filesystem_error{6}};
		return fileMetadata(entryNumber + 1);
	}

	thin_io::filesystem_result<thin_io::filesystem_space> getFilesystemSpace(const NativePath&)
	{
		const uint64_t call = m_spaceCalls.fetch_add(1, std::memory_order_relaxed);
		if (call % 2 == 1)
			blockIfRequested(BlockPoint::completion_space);
		return thin_io::filesystem_space{100000, 50000 - call, 45000 - call, 7};
	}

	bool waitUntilBlocked()
	{
		std::unique_lock lock{m_blockMutex};
		return m_blockCondition.wait_for(lock, std::chrono::seconds{2}, [this] { return m_blocked; });
	}

	void release()
	{
		std::lock_guard lock{m_blockMutex};
		m_released = true;
		m_blockCondition.notify_all();
	}

private:
	void blockIfRequested(const BlockPoint point)
	{
		if (m_blockPoint != point)
			return;
		std::unique_lock lock{m_blockMutex};
		m_blocked = true;
		m_blockCondition.notify_all();
		m_blockCondition.wait(lock, [this] { return m_released; });
	}

	const size_t m_entryCount;
	const FilesystemBehavior m_behavior;
	const BlockPoint m_blockPoint;
	std::atomic_uint64_t m_childMetadataCalls = 0;
	std::atomic_uint64_t m_spaceCalls = 0;
	std::mutex m_blockMutex;
	std::condition_variable m_blockCondition;
	bool m_blocked = false;
	bool m_released = false;
};

struct PublishedEvents
{
	std::vector<std::pair<uint64_t, SnapshotScanProgress>> progress;
	std::vector<std::pair<uint64_t, std::shared_ptr<const SnapshotScanResult>>> completions;
	std::vector<char> order;

	SnapshotScanRunnerCallbacks callbacks()
	{
		return {
			[this](const uint64_t generation, const SnapshotScanProgress& value) {
				progress.emplace_back(generation, value);
				order.push_back('p');
			},
			[this](const uint64_t generation, const std::shared_ptr<const SnapshotScanResult>& result) {
				completions.emplace_back(generation, result);
				order.push_back('c');
			}
		};
	}
};

bool waitUntilIdle(const SnapshotScanRunner& runner)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
	while (runner.scanInProgress() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	return !runner.scanInProgress();
}

const SnapshotScanResult& onlyCompletion(const PublishedEvents& events)
{
	REQUIRE(events.completions.size() == 1);
	return *events.completions.front().second;
}

} // namespace

TEST_CASE("Snapshot scan runner publishes progress before one immutable completion", "[snapshot][scan-runner]")
{
	ControlledFilesystem filesystem{20, FilesystemBehavior::success, BlockPoint::root_enumeration};
	ScopedTestFilesystemAccess filesystemBinding{filesystem};
	CExecutionQueue queue;
	PublishedEvents events;
	SnapshotScanRunner runner{queue, events.callbacks()};

	const auto generation = runner.start(rootPath());
	REQUIRE(generation == 1);
	REQUIRE(filesystem.waitUntilBlocked());
	CHECK_FALSE(runner.start(rootPath()));
	filesystem.release();
	REQUIRE(waitUntilIdle(runner));
	CHECK(events.completions.empty());
	queue.exec();

	REQUIRE(events.progress.size() == 1);
	CHECK(events.progress.front().first == *generation);
	CHECK(events.progress.front().second == (SnapshotScanProgress{1, 20, 0}));
	REQUIRE(events.order.size() == 2);
	CHECK(events.order[0] == 'p');
	CHECK(events.order[1] == 'c');
	CHECK(std::holds_alternative<Snapshot>(onlyCompletion(events)));
}

TEST_CASE("Snapshot scan runner preserves fatal and recoverable outcomes", "[snapshot][scan-runner]")
{
	SECTION("recoverable scan damage still completes")
	{
		ControlledFilesystem filesystem{4, FilesystemBehavior::recoverable_metadata_failure};
		ScopedTestFilesystemAccess filesystemBinding{filesystem};
		CExecutionQueue queue;
		PublishedEvents events;
		SnapshotScanRunner runner{queue, events.callbacks()};
		REQUIRE(runner.start(rootPath()));
		REQUIRE(waitUntilIdle(runner));
		queue.exec();

		const auto* snapshot = std::get_if<Snapshot>(&onlyCompletion(events));
		REQUIRE(snapshot);
		CHECK(snapshot->diagnostics.size() == 1);
		REQUIRE(events.progress.size() == 1);
		CHECK(events.progress.front().second.issues == 1);
	}

	SECTION("fatal scan failure stays typed")
	{
		ControlledFilesystem filesystem{0, FilesystemBehavior::fatal_root_failure};
		ScopedTestFilesystemAccess filesystemBinding{filesystem};
		CExecutionQueue queue;
		PublishedEvents events;
		SnapshotScanRunner runner{queue, events.callbacks()};
		REQUIRE(runner.start(rootPath()));
		REQUIRE(waitUntilIdle(runner));
		queue.exec();

		const auto* failure = std::get_if<SnapshotScanFailure>(&onlyCompletion(events));
		REQUIRE(failure);
		CHECK(failure->code == SnapshotScanFailureCode::root_metadata_unavailable);
	}

	SECTION("unexpected exceptions cannot escape the scan thread")
	{
		ControlledFilesystem filesystem{0, FilesystemBehavior::throw_unexpectedly};
		ScopedTestFilesystemAccess filesystemBinding{filesystem};
		CExecutionQueue queue;
		PublishedEvents events;
		SnapshotScanRunner runner{queue, events.callbacks()};
		REQUIRE(runner.start(rootPath()));
		REQUIRE(waitUntilIdle(runner));
		queue.exec();

		const auto* failure = std::get_if<SnapshotScanFailure>(&onlyCompletion(events));
		REQUIRE(failure);
		CHECK(failure->code == SnapshotScanFailureCode::unexpected_error);
	}
}

TEST_CASE("Snapshot scan runner cancellation is nonblocking and terminal", "[snapshot][scan-runner]")
{
	ControlledFilesystem filesystem{10, FilesystemBehavior::success, BlockPoint::root_enumeration};
	ScopedTestFilesystemAccess filesystemBinding{filesystem};
	CExecutionQueue queue;
	PublishedEvents events;
	SnapshotScanRunner runner{queue, events.callbacks()};
	REQUIRE(runner.start(rootPath()));
	REQUIRE(filesystem.waitUntilBlocked());

	auto cancellation = std::async(std::launch::async, [&runner] { return runner.cancel(); });
	CHECK(cancellation.wait_for(std::chrono::milliseconds{500}) == std::future_status::ready);
	filesystem.release();
	CHECK(cancellation.get());
	REQUIRE(waitUntilIdle(runner));
	CHECK_FALSE(runner.cancel());
	queue.exec();
	CHECK(std::holds_alternative<SnapshotScanCanceled>(onlyCompletion(events)));
}

TEST_CASE("Snapshot scan generations let receivers discard stale publication", "[snapshot][scan-runner]")
{
	ControlledFilesystem filesystem;
	ScopedTestFilesystemAccess filesystemBinding{filesystem};
	CExecutionQueue queue;
	uint64_t currentGeneration = 0;
	std::vector<uint64_t> adoptedGenerations;
	SnapshotScanRunner runner{queue, {
		{},
		[&](const uint64_t generation, const std::shared_ptr<const SnapshotScanResult>&) {
			if (generation == currentGeneration)
				adoptedGenerations.push_back(generation);
		}
	}};

	const auto firstGeneration = runner.start(rootPath());
	REQUIRE(firstGeneration);
	currentGeneration = *firstGeneration;
	REQUIRE(waitUntilIdle(runner));
	const auto secondGeneration = runner.start(rootPath());
	REQUIRE(secondGeneration);
	currentGeneration = *secondGeneration;
	REQUIRE(waitUntilIdle(runner));
	queue.exec();

	REQUIRE(adoptedGenerations.size() == 1);
	CHECK(adoptedGenerations.front() == 2);
}

TEST_CASE("Snapshot scan runner destruction is safe at every scan-thread phase", "[snapshot][scan-runner]")
{
	SECTION("completion already queued")
	{
		ControlledFilesystem filesystem;
		ScopedTestFilesystemAccess filesystemBinding{filesystem};
		CExecutionQueue queue;
		PublishedEvents events;
		{
			SnapshotScanRunner runner{queue, events.callbacks()};
			REQUIRE(runner.start(rootPath()));
			REQUIRE(waitUntilIdle(runner));
		}
		queue.exec();
		CHECK(events.completions.size() == 1);
	}

	for (const BlockPoint blockPoint : {BlockPoint::root_enumeration, BlockPoint::completion_space})
	{
		DYNAMIC_SECTION("active at block point " << static_cast<int>(blockPoint))
		{
			ControlledFilesystem filesystem{4, FilesystemBehavior::success, blockPoint};
			ScopedTestFilesystemAccess filesystemBinding{filesystem};
			CExecutionQueue queue;
			PublishedEvents events;
			auto runner = std::make_unique<SnapshotScanRunner>(queue, events.callbacks());
			REQUIRE(runner->start(rootPath()));
			REQUIRE(filesystem.waitUntilBlocked());
			REQUIRE(runner->cancel());
			auto destruction = std::async(std::launch::async, [&runner] { runner.reset(); });
			CHECK(destruction.wait_for(std::chrono::milliseconds{50}) == std::future_status::timeout);
			filesystem.release();
			destruction.get();
			queue.exec();
			CHECK(std::holds_alternative<SnapshotScanCanceled>(onlyCompletion(events)));
		}
	}
}
