#pragma once

#include "native_path.h"

#include "filesystem_error.hpp"
#include "filesystem_types.hpp"

#include <QDateTime>
#include <QString>

#include <expected>
#include <map>
#include <optional>
#include <stdint.h>
#include <vector>

namespace SpaceGuard {

enum class SnapshotPlatform : uint8_t {
	windows = 1,
	macos,
	linux_os,
	freebsd
};

enum class DirectoryTraversalState : uint8_t {
	not_directory,
	completed,
	enumeration_failed,
	metadata_unavailable,
	link_boundary,
	filesystem_boundary
};

enum class SnapshotOperation : uint8_t {
	root_metadata,
	directory_enumeration,
	entry_metadata,
	filesystem_space_at_start,
	filesystem_space_at_completion
};

struct SnapshotEntryMetadata
{
	uint64_t logicalSize = 0;
	uint64_t allocatedSize = 0;
	uint64_t hardLinkCount = 0;
	std::optional<thin_io::entry_identity> identity;

	[[nodiscard]] bool operator==(const SnapshotEntryMetadata&) const = default;
};

struct SnapshotEntry
{
	thin_io::entry_attributes attributes;
	std::optional<SnapshotEntryMetadata> metadata;
	DirectoryTraversalState traversalState = DirectoryTraversalState::not_directory;
	std::map<NativeName, SnapshotEntry> children;

	[[nodiscard]] bool operator==(const SnapshotEntry&) const = default;
};

struct SnapshotDiagnostic
{
	NativePath path;
	SnapshotOperation operation = SnapshotOperation::entry_metadata;
	thin_io::filesystem_error_code nativeErrorCode = 0;

	[[nodiscard]] bool operator==(const SnapshotDiagnostic&) const = default;
};

enum class SnapshotSaveErrorCode : uint8_t {
	invalid_snapshot,
	serialization_failed,
	open_failed,
	write_failed,
	commit_failed
};

struct SnapshotSaveError
{
	SnapshotSaveErrorCode code;
	QString systemMessage;

	[[nodiscard]] bool operator==(const SnapshotSaveError&) const = default;
};

enum class SnapshotLoadErrorCode : uint8_t {
	open_failed,
	read_failed,
	unsupported_legacy_format,
	unsupported_version,
	wrong_platform,
	decompression_failed,
	truncated,
	corrupt_data,
	trailing_data
};

struct SnapshotLoadError
{
	SnapshotLoadErrorCode code;
	QString systemMessage;

	[[nodiscard]] bool operator==(const SnapshotLoadError&) const = default;
};

struct Snapshot
{
	static constexpr uint16_t CurrentFormatVersion = 1;

	NativePath rootPath;
	SnapshotEntry root;
	std::optional<thin_io::filesystem_space> filesystemSpaceAtStart;
	std::optional<thin_io::filesystem_space> filesystemSpaceAtCompletion;
	QDateTime scanStartedAtUtc;
	QDateTime scanCompletedAtUtc;
	std::vector<SnapshotDiagnostic> diagnostics;

	[[nodiscard]] std::expected<void, SnapshotSaveError> save(const QString& path) const;
	[[nodiscard]] static std::expected<Snapshot, SnapshotLoadError> load(const QString& path);

	[[nodiscard]] bool operator==(const Snapshot&) const = default;
};

[[nodiscard]] SnapshotPlatform currentSnapshotPlatform() noexcept;

} // namespace SpaceGuard
