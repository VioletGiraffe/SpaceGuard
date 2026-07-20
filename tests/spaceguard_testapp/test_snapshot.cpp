#include "3rdparty/catch2/catch.hpp"

#include "snapshot.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <string>
#include <utility>

namespace {

constexpr qsizetype SnapshotHeaderSize = 15;

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

thin_io::entry_identity identity(const uint64_t filesystem, const uint8_t seed)
{
	thin_io::entry_identity result;
	result.filesystem = filesystem;
	for (size_t i = 0; i < result.entry.size(); ++i)
		result.entry[i] = static_cast<uint8_t>(seed + i);
	return result;
}

SnapshotEntryMetadata metadata(const uint64_t logicalSize, const uint64_t allocatedSize,
	const uint64_t hardLinkCount, std::optional<thin_io::entry_identity> entryIdentity = {})
{
	return {logicalSize, allocatedSize, hardLinkCount, std::move(entryIdentity)};
}

SnapshotEntry fileEntry(const uint64_t logicalSize, const uint64_t allocatedSize, const thin_io::entry_identity& entryIdentity)
{
	SnapshotEntry entry;
	entry.attributes = {thin_io::entry_kind::regular_file, false, true, true, 0};
	entry.metadata = metadata(logicalSize, allocatedSize, 2, entryIdentity);
	return entry;
}

SnapshotEntry directoryEntry(const DirectoryTraversalState state, std::optional<SnapshotEntryMetadata> entryMetadata)
{
	SnapshotEntry entry;
	entry.attributes.kind = thin_io::entry_kind::directory;
	entry.metadata = std::move(entryMetadata);
	entry.traversalState = state;
	return entry;
}

Snapshot makeSnapshot(const bool reverseInsertionOrder = false)
{
	constexpr uint64_t filesystem = 42;
	Snapshot snapshot;
#ifdef _WIN32
	snapshot.rootPath = QString::fromWCharArray(L"C:\\scan-\u0434\u0430\u043d\u0456");
#else
	snapshot.rootPath = "/scan-data";
#endif
	snapshot.root = directoryEntry(DirectoryTraversalState::completed, metadata(0, 4096, 1, identity(filesystem, 1)));

	SnapshotEntry completed = directoryEntry(DirectoryTraversalState::completed, metadata(0, 4096, 1, identity(filesystem, 2)));
	completed.children.try_emplace(nativeName("other"), SnapshotEntry{{thin_io::entry_kind::other, false, false, false, 0}, metadata(7, 8, 1)});

	SnapshotEntry failed = directoryEntry(DirectoryTraversalState::enumeration_failed, metadata(0, 4096, 1, identity(filesystem, 3)));
	SnapshotEntry unknownMetadata = directoryEntry(DirectoryTraversalState::metadata_unavailable, {});
	SnapshotEntry link = directoryEntry(DirectoryTraversalState::link_boundary, metadata(0, 0, 1));
	link.attributes.is_link = true;
#ifdef _WIN32
	link.attributes.reparse_tag = 0xA000000C;
#endif
	SnapshotEntry boundary = directoryEntry(DirectoryTraversalState::mount_boundary, metadata(0, 4096, 1, identity(99, 4)));
	SnapshotEntry unknown;
	unknown.attributes.kind = thin_io::entry_kind::unknown;

#ifdef _WIN32
	const NativeName unusualName = QString::fromWCharArray(L"unicode-\u0416");
#else
	NativeName unusualName{"raw-"};
	unusualName.push_back(static_cast<char>(0xFF));
#endif

	using NamedEntry = std::pair<NativeName, SnapshotEntry>;
	std::array entries{
		NamedEntry{nativeName("complete"), std::move(completed)},
		NamedEntry{nativeName("failed"), std::move(failed)},
		NamedEntry{nativeName("unknown-metadata"), std::move(unknownMetadata)},
		NamedEntry{nativeName("link"), std::move(link)},
		NamedEntry{nativeName("boundary"), std::move(boundary)},
		NamedEntry{nativeName("unknown"), std::move(unknown)},
		NamedEntry{std::move(unusualName), fileEntry(8192, 4096, identity(filesystem, 5))}
	};
	if (reverseInsertionOrder)
		std::ranges::reverse(entries);
	for (auto& [name, entry] : entries)
		snapshot.root.children.try_emplace(std::move(name), std::move(entry));

	snapshot.filesystemSpaceAtStart = thin_io::filesystem_space{100000, 40000, 30000, filesystem};
	snapshot.filesystemSpaceAtCompletion = thin_io::filesystem_space{100000, 35000, 25000, {}};
	snapshot.scanStartedAtUtc = QDateTime::fromMSecsSinceEpoch(1000, QTimeZone::UTC);
	snapshot.scanCompletedAtUtc = QDateTime::fromMSecsSinceEpoch(2000, QTimeZone::UTC);

	const std::array operations{
		SnapshotOperation::root_metadata,
		SnapshotOperation::directory_enumeration,
		SnapshotOperation::entry_metadata,
		SnapshotOperation::filesystem_space_at_start,
		SnapshotOperation::filesystem_space_at_completion,
		SnapshotOperation::entry_changed_during_scan
	};
	for (size_t i = 0; i < operations.size(); ++i)
		snapshot.diagnostics.push_back({snapshot.rootPath, operations[i], operations[i] == SnapshotOperation::entry_changed_during_scan
			? std::nullopt : std::optional{static_cast<thin_io::filesystem_error_code>(5 + i)}});
	return snapshot;
}

QByteArray readFile(const QString& path)
{
	QFile file{path};
	REQUIRE(file.open(QIODevice::ReadOnly));
	return file.readAll();
}

void writeFile(const QString& path, const QByteArray& data)
{
	QFile file{path};
	REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
	REQUIRE(file.write(data) == data.size());
}

QByteArray replaceCompressedPayload(const QByteArray& fileData, const QByteArray& compressed)
{
	REQUIRE(fileData.size() >= SnapshotHeaderSize);
	QByteArray result = fileData.first(SnapshotHeaderSize) + compressed;
	qToLittleEndian<quint32>(static_cast<quint32>(compressed.size()), reinterpret_cast<uchar*>(result.data() + 11));
	return result;
}

QByteArray replacePayload(const QByteArray& fileData, const QByteArray& payload)
{
	return replaceCompressedPayload(fileData, qCompress(payload, 3));
}

QByteArray uncompressedPayload(const QByteArray& fileData)
{
	REQUIRE(fileData.size() >= SnapshotHeaderSize);
	const QByteArray payload = qUncompress(fileData.sliced(SnapshotHeaderSize));
	REQUIRE_FALSE(payload.isEmpty());
	return payload;
}

void checkLoadError(const QString& path, const QByteArray& data, const SnapshotLoadErrorCode expectedError)
{
	writeFile(path, data);
	const auto result = Snapshot::load(path);
	REQUIRE_FALSE(result);
	CHECK(result.error().code == expectedError);
}

qsizetype rootKindOffset(const Snapshot& snapshot)
{
#ifdef _WIN32
	return sizeof(quint32) + snapshot.rootPath.size() * sizeof(quint16);
#else
	return sizeof(quint32) + snapshot.rootPath.size();
#endif
}

qsizetype rootTraversalStateOffset(const Snapshot& snapshot)
{
	constexpr qsizetype AttributesSize = 8;
	constexpr qsizetype MetadataSize = 3 * sizeof(quint64);
	constexpr qsizetype IdentitySize = sizeof(quint64) + 16;
	return rootKindOffset(snapshot) + AttributesSize + 1 + MetadataSize + 1 + (snapshot.root.metadata->identity ? IdentitySize : 0);
}

} // namespace

TEST_CASE("Snapshots preserve all factual fields", "[snapshot][persistence]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath("snapshot.spaceguard");
	const Snapshot original = makeSnapshot();

	REQUIRE(original.save(path));
	const auto loaded = Snapshot::load(path);
	REQUIRE(loaded);
	CHECK(*loaded == original);
	CHECK(loaded->derivedDataAvailable);

	Snapshot withoutOptionalRootFacts = original;
	withoutOptionalRootFacts.filesystemSpaceAtStart.reset();
	withoutOptionalRootFacts.filesystemSpaceAtCompletion.reset();
	withoutOptionalRootFacts.diagnostics.clear();
	const QString optionalPath = directory.filePath("without-optional-fields.spaceguard");
	REQUIRE(withoutOptionalRootFacts.save(optionalPath));
	const auto optionalLoaded = Snapshot::load(optionalPath);
	REQUIRE(optionalLoaded);
	CHECK(*optionalLoaded == withoutOptionalRootFacts);
	CHECK(optionalLoaded->derivedDataAvailable);
}

TEST_CASE("Snapshot serialization is deterministic", "[snapshot][persistence]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString firstPath = directory.filePath("first.spaceguard");
	const QString secondPath = directory.filePath("second.spaceguard");

	REQUIRE(makeSnapshot(false).save(firstPath));
	REQUIRE(makeSnapshot(true).save(secondPath));
	CHECK(readFile(firstPath) == readFile(secondPath));
}

TEST_CASE("Large snapshots round-trip", "[snapshot][persistence]")
{
	constexpr size_t EntryCount = 50000;
	constexpr size_t DiagnosticCount = 2048;
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath("large.spaceguard");

	Snapshot original = makeSnapshot();
	original.root.children.clear();
	original.diagnostics.clear();
	original.root.children.reserve(EntryCount);
	original.root.children.begin_batch();
	for (size_t i = 0; i < EntryCount; ++i)
	{
		SnapshotEntry entry;
		entry.attributes = {thin_io::entry_kind::regular_file, false, true, true, 0};
		entry.metadata = metadata(i + 1, (i + 1) * 4096, 1);
		const std::string name = "file-" + std::to_string(i);
		original.root.children.append_unsorted(nativeName(name.c_str()), std::move(entry));
	}
	original.root.children.end_batch();
	for (size_t i = 0; i < DiagnosticCount; ++i)
		original.diagnostics.push_back({original.rootPath, SnapshotOperation::entry_changed_during_scan, {}});

	REQUIRE(original.save(path));
	const auto loaded = Snapshot::load(path);
	REQUIRE(loaded);
	CHECK(*loaded == original);
	CHECK(loaded->root.children.size() == EntryCount);
	CHECK(loaded->diagnostics.size() == DiagnosticCount);
	CHECK(loaded->derivedDataAvailable);
}

TEST_CASE("Snapshot loader rejects incompatible formats before reading the payload", "[snapshot][persistence]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath("snapshot.spaceguard");
	REQUIRE(makeSnapshot().save(path));
	const QByteArray valid = readFile(path);

	checkLoadError(path, qCompress("prototype snapshot"), SnapshotLoadErrorCode::unsupported_legacy_format);
	checkLoadError(path, valid.first(4), SnapshotLoadErrorCode::truncated);

	QByteArray old = valid;
	qToLittleEndian<quint16>(1, reinterpret_cast<uchar*>(old.data() + 8));
	checkLoadError(path, old, SnapshotLoadErrorCode::unsupported_version);

	QByteArray future = valid;
	qToLittleEndian<quint16>(Snapshot::CurrentFormatVersion + 1, reinterpret_cast<uchar*>(future.data() + 8));
	checkLoadError(path, future, SnapshotLoadErrorCode::unsupported_version);

	for (const SnapshotPlatform platform : {SnapshotPlatform::windows, SnapshotPlatform::macos,
		 SnapshotPlatform::linux_os, SnapshotPlatform::freebsd})
	{
		if (platform == currentSnapshotPlatform())
			continue;
		QByteArray otherPlatform = valid;
		otherPlatform[10] = static_cast<char>(platform);
		checkLoadError(path, otherPlatform, SnapshotLoadErrorCode::wrong_platform);
	}

	QByteArray invalidPlatform = valid;
	invalidPlatform[10] = static_cast<char>(0xFF);
	checkLoadError(path, invalidPlatform, SnapshotLoadErrorCode::corrupt_data);
}

TEST_CASE("Snapshot loader distinguishes damaged payloads", "[snapshot][persistence]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath("snapshot.spaceguard");
	const Snapshot snapshot = makeSnapshot();
	REQUIRE(snapshot.save(path));
	const QByteArray valid = readFile(path);

	checkLoadError(path, replaceCompressedPayload(valid, QByteArray(4, '\0') + "broken"), SnapshotLoadErrorCode::decompression_failed);
	checkLoadError(path, valid.first(valid.size() - 1), SnapshotLoadErrorCode::truncated);

	QByteArray truncatedPayload = uncompressedPayload(valid);
	truncatedPayload.chop(1);
	checkLoadError(path, replacePayload(valid, truncatedPayload), SnapshotLoadErrorCode::truncated);

	QByteArray trailingPayload = uncompressedPayload(valid);
	trailingPayload += 'x';
	checkLoadError(path, replacePayload(valid, trailingPayload), SnapshotLoadErrorCode::trailing_data);
	checkLoadError(path, valid + 'x', SnapshotLoadErrorCode::trailing_data);

	QByteArray invalidEnumPayload = uncompressedPayload(valid);
	invalidEnumPayload[rootKindOffset(snapshot)] = static_cast<char>(0xFF);
	checkLoadError(path, replacePayload(valid, invalidEnumPayload), SnapshotLoadErrorCode::corrupt_data);

	invalidEnumPayload = uncompressedPayload(valid);
	invalidEnumPayload[rootTraversalStateOffset(snapshot)] = static_cast<char>(0xFF);
	checkLoadError(path, replacePayload(valid, invalidEnumPayload), SnapshotLoadErrorCode::corrupt_data);

	invalidEnumPayload = uncompressedPayload(valid);
	invalidEnumPayload[invalidEnumPayload.size() - 2] = static_cast<char>(0xFF);
	checkLoadError(path, replacePayload(valid, invalidEnumPayload), SnapshotLoadErrorCode::corrupt_data);

	QByteArray oversizedCountPayload = uncompressedPayload(valid);
	std::fill_n(oversizedCountPayload.begin() + rootTraversalStateOffset(snapshot) + 1, sizeof(quint32), static_cast<char>(0xFF));
	checkLoadError(path, replacePayload(valid, oversizedCountPayload), SnapshotLoadErrorCode::corrupt_data);

	QByteArray oversizedStringPayload = uncompressedPayload(valid);
	std::fill_n(oversizedStringPayload.begin(), sizeof(quint32), static_cast<char>(0xFF));
	checkLoadError(path, replacePayload(valid, oversizedStringPayload), SnapshotLoadErrorCode::corrupt_data);

	Snapshot withoutDiagnostics = snapshot;
	withoutDiagnostics.diagnostics.clear();
	REQUIRE(withoutDiagnostics.save(path));
	const QByteArray withoutDiagnosticsFile = readFile(path);
	QByteArray oversizedDiagnosticCountPayload = uncompressedPayload(withoutDiagnosticsFile);
	REQUIRE(oversizedDiagnosticCountPayload.size() >= static_cast<qsizetype>(sizeof(quint32)));
	const qsizetype diagnosticCountOffset = oversizedDiagnosticCountPayload.size() - static_cast<qsizetype>(sizeof(quint32));
	std::fill_n(oversizedDiagnosticCountPayload.begin() + diagnosticCountOffset, sizeof(quint32), static_cast<char>(0xFF));
	checkLoadError(path, replacePayload(withoutDiagnosticsFile, oversizedDiagnosticCountPayload), SnapshotLoadErrorCode::corrupt_data);

	QByteArray inconsistentPayload = uncompressedPayload(valid);
	inconsistentPayload[rootKindOffset(snapshot) + 4] = 1;
	checkLoadError(path, replacePayload(valid, inconsistentPayload), SnapshotLoadErrorCode::corrupt_data);
}

TEST_CASE("Snapshot load and save failures are transactional", "[snapshot][persistence]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());
	const QString path = directory.filePath("snapshot.spaceguard");
	const Snapshot original = makeSnapshot();
	REQUIRE(original.save(path));
	const QByteArray originalBytes = readFile(path);

	Snapshot invalid = original;
	invalid.rootPath.clear();
	const auto saveResult = invalid.save(path);
	REQUIRE_FALSE(saveResult);
	CHECK(saveResult.error().code == SnapshotSaveErrorCode::invalid_snapshot);
	CHECK(readFile(path) == originalBytes);

	invalid = original;
	invalid.filesystemSpaceAtStart = thin_io::filesystem_space{100, 200, 150, 42};
	const auto invalidSpaceResult = invalid.save(path);
	REQUIRE_FALSE(invalidSpaceResult);
	CHECK(invalidSpaceResult.error().code == SnapshotSaveErrorCode::invalid_snapshot);
	CHECK(readFile(path) == originalBytes);

	const Snapshot current = original;
	writeFile(path, QByteArray{"not a snapshot"});
	const auto loadResult = Snapshot::load(path);
	REQUIRE_FALSE(loadResult);
	CHECK(current == original);

	const auto missing = Snapshot::load(directory.filePath("missing.spaceguard"));
	REQUIRE_FALSE(missing);
	CHECK(missing.error().code == SnapshotLoadErrorCode::open_failed);

	const auto invalidDestination = original.save(directory.filePath("missing/snapshot.spaceguard"));
	REQUIRE_FALSE(invalidDestination);
	CHECK(invalidDestination.error().code == SnapshotSaveErrorCode::open_failed);
	CHECK_FALSE(invalidDestination.error().systemMessage.isEmpty());
}
