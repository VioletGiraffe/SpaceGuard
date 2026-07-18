#pragma once

#include "filesystem_access.h"
#include "snapshot.h"

#include <atomic>
#include <optional>
#include <variant>

namespace SpaceGuard {

enum class SnapshotScanFailureCode : uint8_t {
	invalid_root,
	root_metadata_unavailable,
	root_not_directory,
	root_is_link,
	filesystem_space_at_start_unavailable,
	root_filesystem_identity_mismatch,
	root_enumeration_unavailable,
	root_filesystem_identity_changed
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

using SnapshotScanResult = std::variant<Snapshot, SnapshotScanFailure, SnapshotScanCanceled>;

[[nodiscard]] SnapshotScanResult scanSnapshot(
	const NativePath& normalizedRootPath, FilesystemAccess& filesystem, const std::atomic_bool& canceled);

} // namespace SpaceGuard
