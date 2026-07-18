#pragma once

#include "snapshot.h"

#include <atomic>
#include <functional>
#include <optional>
#include <stdint.h>
#include <variant>

class CWorkerThreadPool;

enum class SnapshotScanFailureCode : uint8_t {
	invalid_root,
	root_metadata_unavailable,
	root_not_directory,
	root_is_link,
	filesystem_space_at_start_unavailable,
	root_filesystem_identity_mismatch,
	root_enumeration_unavailable,
	root_filesystem_identity_changed,
	unexpected_error
};

struct SnapshotScanFailure
{
	SnapshotScanFailureCode code;
	NativePath path;
	std::optional<thin_io::filesystem_error_code> nativeErrorCode;

	[[nodiscard]] bool operator==(const SnapshotScanFailure&) const = default;
};

struct SnapshotScanCanceled
{
	[[nodiscard]] bool operator==(const SnapshotScanCanceled&) const = default;
};

struct SnapshotScanProgress
{
	uint64_t directoriesCompleted = 0;
	uint64_t entriesDiscovered = 0;
	uint64_t issues = 0;

	[[nodiscard]] bool operator==(const SnapshotScanProgress&) const = default;
};

using SnapshotScanResult = std::variant<Snapshot, SnapshotScanFailure, SnapshotScanCanceled>;
using SnapshotScanProgressCallback = std::function<void(const SnapshotScanProgress&)>;

// Runs traversal entirely on the calling thread.
[[nodiscard]] SnapshotScanResult scanSnapshot(
	const NativePath& normalizedRootPath, const std::atomic_bool& canceled,
	SnapshotScanProgressCallback progressCallback = {});

// The calling thread participates, so maxWorkersCount() is the total traversal participant count.
[[nodiscard]] SnapshotScanResult scanSnapshot(
	const NativePath& normalizedRootPath, const std::atomic_bool& canceled, CWorkerThreadPool& workerPool,
	SnapshotScanProgressCallback progressCallback = {});
