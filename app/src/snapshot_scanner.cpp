#include "snapshot_scanner.h"

#include <QDateTime>

#include <assert.h>
#include <utility>

namespace SpaceGuard {
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
	Scanner(FilesystemAccess& filesystem, const std::atomic_bool& canceled, SnapshotScanProgressCallback progressCallback)
		: m_filesystem{filesystem}, m_canceled{canceled}, m_progressCallback{std::move(progressCallback)}
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

		const auto rootMetadata = m_filesystem.getEntryMetadata(rootPath, thin_io::link_behavior::do_not_follow);
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

		const auto startSpace = m_filesystem.getFilesystemSpace(rootPath);
		if (m_canceled.load(std::memory_order_relaxed))
			return SnapshotScanCanceled{};
		if (!startSpace)
			return scanFailure(SnapshotScanFailureCode::filesystem_space_at_start_unavailable, rootPath, startSpace.error().native_code);
		if (m_rootFilesystemIdentity && startSpace->identity && *m_rootFilesystemIdentity != *startSpace->identity)
			return scanFailure(SnapshotScanFailureCode::root_filesystem_identity_mismatch, rootPath);
		m_snapshot.filesystemSpaceAtStart = *startSpace;
		if (!m_rootFilesystemIdentity)
			m_rootFilesystemIdentity = startSpace->identity;

		if (const auto rootFailure = scanDirectory(rootPath, m_snapshot.root, true))
			return *rootFailure;
		if (m_canceled.load(std::memory_order_relaxed))
			return SnapshotScanCanceled{};

		const auto completionSpace = m_filesystem.getFilesystemSpace(rootPath);
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
		m_snapshot.rebuildDerivedData();
		return std::move(m_snapshot);
	}

private:
	std::optional<SnapshotScanFailure> scanDirectory(const NativePath& path, SnapshotEntry& directory, const bool isRoot)
	{
		if (m_canceled.load(std::memory_order_relaxed))
			return {};

		const auto entries = m_filesystem.listDirectory(path);
		if (m_canceled.load(std::memory_order_relaxed))
			return {};
		if (!entries)
		{
			if (isRoot)
				return scanFailure(SnapshotScanFailureCode::root_enumeration_unavailable, path, entries.error().native_code);
			directory.traversalState = DirectoryTraversalState::enumeration_failed;
			recordDiagnostic(path, SnapshotOperation::directory_enumeration, entries.error().native_code);
			completeDirectory();
			return {};
		}

		for (const thin_io::directory_entry& listedEntry : *entries)
		{
			if (m_canceled.load(std::memory_order_relaxed))
				return {};
			SnapshotEntry child;
			child.attributes = listedEntry.attributes;
			const bool inserted = directory.children.emplace(nativeNameFromThinIo(listedEntry.name), std::move(child)).second;
			assert(inserted);
			(void)inserted;
		}
		m_progress.entriesDiscovered += static_cast<uint64_t>(entries->size());
		reportProgress();

		for (auto& [name, child] : directory.children)
		{
			if (m_canceled.load(std::memory_order_relaxed))
				return {};
			const NativePath childPath = appendNativeName(path, name);
			const auto metadata = m_filesystem.getEntryMetadata(childPath, thin_io::link_behavior::do_not_follow);
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
			if (const auto childFailure = scanDirectory(childPath, child, false))
				return childFailure;
		}

		if (!m_canceled.load(std::memory_order_relaxed))
		{
			directory.traversalState = DirectoryTraversalState::completed;
			completeDirectory();
		}
		return {};
	}

	void recordDiagnostic(const NativePath& path, const SnapshotOperation operation,
		const std::optional<thin_io::filesystem_error_code> nativeErrorCode)
	{
		m_snapshot.diagnostics.push_back({path, operation, nativeErrorCode});
		++m_progress.issues;
		reportProgress();
	}

	void completeDirectory()
	{
		++m_progress.directoriesCompleted;
		reportProgress();
	}

	void reportProgress() const
	{
		if (m_progressCallback)
			m_progressCallback(m_progress);
	}

	FilesystemAccess& m_filesystem;
	const std::atomic_bool& m_canceled;
	SnapshotScanProgressCallback m_progressCallback;
	Snapshot m_snapshot;
	SnapshotScanProgress m_progress;
	std::optional<thin_io::filesystem_identity> m_rootFilesystemIdentity;
};

} // namespace

SnapshotScanResult scanSnapshot(
	const NativePath& normalizedRootPath, FilesystemAccess& filesystem, const std::atomic_bool& canceled,
	SnapshotScanProgressCallback progressCallback)
{
	return Scanner{filesystem, canceled, std::move(progressCallback)}.scan(normalizedRootPath);
}

} // namespace SpaceGuard
