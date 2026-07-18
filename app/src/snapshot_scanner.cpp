#include "snapshot_scanner.h"

#ifdef SPACEGUARD_TEST_FILESYSTEM_ACCESS
// The separate test executable recompiles this source against its callback-backed adapter.
#include "test_filesystem_access_adapter.h"
using FilesystemAccess = TestFilesystemAccess;
#else
#include "filesystem_access.h"
#endif

#include "threading/cworkerthread.h"

#include <QDateTime>

#include <algorithm>
#include <assert.h>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace {

SnapshotEntryMetadata snapshotMetadata(const thin_io::entry_metadata& metadata)
{
	return {metadata.logical_size, metadata.allocated_size, metadata.hard_link_count, metadata.identity};
}

SnapshotScanFailure scanFailure(const SnapshotScanFailureCode code, const NativePath& path,
	const std::optional<thin_io::filesystem_error_code> nativeErrorCode = {})
{
	return {code, path, nativeErrorCode};
}

void markMetadataUnavailable(SnapshotEntry& entry)
{
	if (entry.attributes.kind == thin_io::entry_kind::directory)
		entry.traversalState = entry.attributes.is_link ? DirectoryTraversalState::link_boundary : DirectoryTraversalState::metadata_unavailable;
}

class Scanner
{
public:
	Scanner(const std::atomic_bool& canceled, SnapshotScanProgressCallback progressCallback, CWorkerThreadPool* workerPool)
		: m_canceled{canceled}, m_progressCallback{std::move(progressCallback)}, m_workerPool{workerPool}
	{
	}

	SnapshotScanResult scan(const NativePath& rootPath)
	{
		if (m_canceled.load(std::memory_order_relaxed))
			return SnapshotScanCanceled{};
		if (!isAbsoluteNativePath(rootPath))
			return scanFailure(SnapshotScanFailureCode::invalid_root, rootPath);

		m_snapshot.rootPath = rootPath;
		m_snapshot.scanStartedAtUtc = QDateTime::currentDateTimeUtc();

		const auto rootMetadata = FilesystemAccess::getEntryMetadata(rootPath, thin_io::link_behavior::do_not_follow);
		if (m_canceled.load(std::memory_order_relaxed))
			return SnapshotScanCanceled{};
		if (!rootMetadata)
			return scanFailure(SnapshotScanFailureCode::root_metadata_unavailable, rootPath, rootMetadata.error().native_code);
		if (rootMetadata->attributes.kind != thin_io::entry_kind::directory)
			return scanFailure(SnapshotScanFailureCode::root_not_directory, rootPath);
		if (rootMetadata->attributes.is_link)
			return scanFailure(SnapshotScanFailureCode::root_is_link, rootPath);

		m_snapshot.root.attributes = rootMetadata->attributes;
		m_snapshot.root.metadata = snapshotMetadata(*rootMetadata);
		if (rootMetadata->identity)
			m_rootFilesystemIdentity = rootMetadata->identity->filesystem;

		const auto startSpace = FilesystemAccess::getFilesystemSpace(rootPath);
		if (m_canceled.load(std::memory_order_relaxed))
			return SnapshotScanCanceled{};
		if (!startSpace)
			return scanFailure(SnapshotScanFailureCode::filesystem_space_at_start_unavailable, rootPath, startSpace.error().native_code);
		if (m_rootFilesystemIdentity && startSpace->identity && *m_rootFilesystemIdentity != *startSpace->identity)
			return scanFailure(SnapshotScanFailureCode::root_filesystem_identity_mismatch, rootPath);
		m_snapshot.filesystemSpaceAtStart = *startSpace;
		if (!m_rootFilesystemIdentity)
			m_rootFilesystemIdentity = startSpace->identity;

		if (const auto rootFailure = scanDirectories(rootPath))
			return *rootFailure;
		if (m_canceled.load(std::memory_order_relaxed))
			return SnapshotScanCanceled{};

		const auto completionSpace = FilesystemAccess::getFilesystemSpace(rootPath);
		if (m_canceled.load(std::memory_order_relaxed))
			return SnapshotScanCanceled{};
		if (!completionSpace)
		{
			recordDiagnostic(rootPath, SnapshotOperation::filesystem_space_at_completion, completionSpace.error().native_code);
		}
		else
		{
			if (m_rootFilesystemIdentity && completionSpace->identity && *m_rootFilesystemIdentity != *completionSpace->identity)
				return scanFailure(SnapshotScanFailureCode::root_filesystem_identity_changed, rootPath);
			m_snapshot.filesystemSpaceAtCompletion = *completionSpace;
		}

		m_snapshot.scanCompletedAtUtc = QDateTime::currentDateTimeUtc();
		std::ranges::sort(m_snapshot.diagnostics, [](const SnapshotDiagnostic& left, const SnapshotDiagnostic& right) {
			const int pathOrder = left.path.compare(right.path);
			if (pathOrder != 0)
				return pathOrder < 0;
			if (left.operation != right.operation)
				return left.operation < right.operation;
			return left.nativeErrorCode < right.nativeErrorCode;
		});
		m_snapshot.rebuildDerivedData();
		return std::move(m_snapshot);
	}

private:
	struct DirectoryWork
	{
		NativePath path;
		SnapshotEntry* entry = nullptr;
		bool isRoot = false;
	};

	std::optional<SnapshotScanFailure> scanDirectories(const NativePath& rootPath)
	{
		m_pendingDirectories.push_back({rootPath, &m_snapshot.root, true});
		m_outstandingDirectories = 1;

		if (m_workerPool)
		{
			const std::size_t participantCount = m_workerPool->maxWorkersCount() + 1;
			m_workerPool->parallelFor(participantCount, [this](const std::size_t) noexcept { processDirectories(); });
		}
		else
		{
			processDirectories();
		}

		assert(m_outstandingDirectories == 0);
		assert(m_pendingDirectories.empty());
		if (m_canceled.load(std::memory_order_relaxed))
			return {};
		if (m_unexpectedError)
			return scanFailure(SnapshotScanFailureCode::unexpected_error, rootPath);
		return std::move(m_fatalFailure);
	}

	void processDirectories() noexcept
	{
		for (;;)
		{
			DirectoryWork directory;
			{
				std::unique_lock lock{m_workMutex};
				// An empty queue with outstanding work implies an active participant. Its completion notification
				// wakes idle participants after the current native call, which is also the cancellation latency bound.
				m_workChanged.wait(lock, [this] {
					return m_canceled.load(std::memory_order_relaxed) || m_unexpectedError || m_fatalFailure
						|| m_outstandingDirectories == 0 || !m_pendingDirectories.empty();
				});
				if (m_canceled.load(std::memory_order_relaxed))
				{
					discardPendingDirectoriesLocked();
					m_workChanged.notify_all();
					return;
				}
				if (m_unexpectedError || m_fatalFailure || m_outstandingDirectories == 0)
					return;

				directory = std::move(m_pendingDirectories.front());
				m_pendingDirectories.pop_front();
			}

			std::vector<DirectoryWork> discoveredDirectories;
			std::optional<SnapshotScanFailure> failure;
			bool unexpectedError = false;
			try
			{
				failure = scanDirectory(directory, discoveredDirectories);
			}
			catch (...)
			{
				unexpectedError = true;
			}
			finishDirectory(std::move(discoveredDirectories), std::move(failure), unexpectedError);
		}
	}

	std::optional<SnapshotScanFailure> scanDirectory(
		const DirectoryWork& work, std::vector<DirectoryWork>& discoveredDirectories)
	{
		if (m_canceled.load(std::memory_order_relaxed))
			return {};

		const auto entries = FilesystemAccess::listDirectory(work.path);
		if (m_canceled.load(std::memory_order_relaxed))
			return {};
		if (!entries)
		{
			if (work.isRoot)
				return scanFailure(SnapshotScanFailureCode::root_enumeration_unavailable, work.path, entries.error().native_code);
			work.entry->traversalState = DirectoryTraversalState::enumeration_failed;
			recordDiagnostic(work.path, SnapshotOperation::directory_enumeration, entries.error().native_code);
			completeDirectory();
			return {};
		}

		for (const thin_io::directory_entry& listedEntry : *entries)
		{
			if (m_canceled.load(std::memory_order_relaxed))
				return {};
			SnapshotEntry child;
			child.attributes = listedEntry.attributes;
			const bool inserted = work.entry->children.emplace(nativeNameFromThinIo(listedEntry.name), std::move(child)).second;
			assert(inserted);
			(void)inserted;
		}
		discoverEntries(static_cast<uint64_t>(entries->size()));

		for (auto& [name, child] : work.entry->children)
		{
			if (m_canceled.load(std::memory_order_relaxed))
				return {};
			NativePath childPath = appendNativeName(work.path, name);
			const auto metadata = FilesystemAccess::getEntryMetadata(childPath, thin_io::link_behavior::do_not_follow);
			if (m_canceled.load(std::memory_order_relaxed))
				return {};
			if (!metadata)
			{
				markMetadataUnavailable(child);
				recordDiagnostic(childPath, SnapshotOperation::entry_metadata, metadata.error().native_code);
				continue;
			}
			if (metadata->attributes != child.attributes)
			{
				markMetadataUnavailable(child);
				recordDiagnostic(childPath, SnapshotOperation::entry_changed_during_scan, {});
				continue;
			}

			child.metadata = snapshotMetadata(*metadata);
			if (child.attributes.kind != thin_io::entry_kind::directory)
				continue;
			if (child.attributes.is_link)
			{
				child.traversalState = DirectoryTraversalState::link_boundary;
				continue;
			}
			if (m_rootFilesystemIdentity && metadata->identity
				&& *m_rootFilesystemIdentity != metadata->identity->filesystem)
			{
				child.traversalState = DirectoryTraversalState::filesystem_boundary;
				continue;
			}
			if (m_canceled.load(std::memory_order_relaxed))
				return {};
			discoveredDirectories.push_back({std::move(childPath), &child, false});
		}

		if (!m_canceled.load(std::memory_order_relaxed))
		{
			work.entry->traversalState = DirectoryTraversalState::completed;
			completeDirectory();
		}
		return {};
	}

	void finishDirectory(std::vector<DirectoryWork> discoveredDirectories,
		std::optional<SnapshotScanFailure> failure, const bool unexpectedError) noexcept
	{
		{
			std::lock_guard lock{m_workMutex};
			try
			{
				if (unexpectedError)
					m_unexpectedError = true;
				else if (failure && !m_fatalFailure)
					m_fatalFailure = std::move(failure);

				if (!m_canceled.load(std::memory_order_relaxed) && !m_unexpectedError && !m_fatalFailure)
				{
					for (DirectoryWork& discovered : discoveredDirectories)
					{
						m_pendingDirectories.push_back(std::move(discovered));
						++m_outstandingDirectories;
					}
				}
			}
			catch (...)
			{
				m_unexpectedError = true;
			}

			if (m_canceled.load(std::memory_order_relaxed) || m_unexpectedError || m_fatalFailure)
				discardPendingDirectoriesLocked();
			assert(m_outstandingDirectories > 0);
			--m_outstandingDirectories;
		}
		m_workChanged.notify_all();
	}

	void discardPendingDirectoriesLocked() noexcept
	{
		assert(m_outstandingDirectories >= m_pendingDirectories.size());
		m_outstandingDirectories -= m_pendingDirectories.size();
		m_pendingDirectories.clear();
	}

	void recordDiagnostic(const NativePath& path, const SnapshotOperation operation,
		const std::optional<thin_io::filesystem_error_code> nativeErrorCode)
	{
		std::lock_guard lock{m_resultMutex};
		m_snapshot.diagnostics.push_back({path, operation, nativeErrorCode});
		++m_progress.issues;
		reportProgressLocked();
	}

	void discoverEntries(const uint64_t count)
	{
		std::lock_guard lock{m_resultMutex};
		m_progress.entriesDiscovered += count;
		reportProgressLocked();
	}

	void completeDirectory()
	{
		std::lock_guard lock{m_resultMutex};
		++m_progress.directoriesCompleted;
		reportProgressLocked();
	}

	void reportProgressLocked() const
	{
		if (m_progressCallback)
			m_progressCallback(m_progress);
	}

	const std::atomic_bool& m_canceled;
	SnapshotScanProgressCallback m_progressCallback;
	Snapshot m_snapshot;
	SnapshotScanProgress m_progress;
	std::optional<thin_io::filesystem_identity> m_rootFilesystemIdentity;
	CWorkerThreadPool* m_workerPool;
	std::mutex m_workMutex;
	std::condition_variable m_workChanged;
	std::deque<DirectoryWork> m_pendingDirectories;
	std::size_t m_outstandingDirectories = 0;
	std::optional<SnapshotScanFailure> m_fatalFailure;
	bool m_unexpectedError = false;
	std::mutex m_resultMutex;
};

} // namespace

SnapshotScanResult scanSnapshot(
	const NativePath& normalizedRootPath, const std::atomic_bool& canceled,
	SnapshotScanProgressCallback progressCallback, CWorkerThreadPool* workerPool)
{
	return Scanner{canceled, std::move(progressCallback), workerPool}.scan(normalizedRootPath);
}
