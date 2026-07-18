#include "snapshot.h"
#include "snapshot_internal.h"

#include <QDataStream>
#include <QFile>
#include <QSaveFile>
#include <QTimeZone>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

constexpr char FileMagic[] = {'S', 'P', 'G', 'U', 'A', 'R', 'D', '\0'};
constexpr qsizetype FileHeaderSize = sizeof(FileMagic) + sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t);
constexpr uint32_t MaximumNativeStringLength = 16 * 1024 * 1024;
constexpr uint32_t MaximumEntryCount = 100 * 1000 * 1000;
constexpr uint32_t MaximumDiagnosticCount = 10 * 1000 * 1000;
constexpr uint32_t MaximumTreeDepth = 1024;

void configureStream(QDataStream& stream)
{
	stream.setVersion(QDataStream::Qt_6_0);
	stream.setByteOrder(QDataStream::LittleEndian);
}

template <class Enum>
void writeEnum(QDataStream& stream, const Enum value)
{
	stream << static_cast<quint8>(value);
}

bool readByte(QDataStream& stream, uint8_t& value)
{
	quint8 serialized = 0;
	stream >> serialized;
	value = serialized;
	return stream.status() == QDataStream::Ok;
}

void writeUint64(QDataStream& stream, const uint64_t value)
{
	stream << static_cast<quint64>(value);
}

bool readUint64(QDataStream& stream, uint64_t& value)
{
	quint64 serialized = 0;
	stream >> serialized;
	value = serialized;
	return stream.status() == QDataStream::Ok;
}

void writeBool(QDataStream& stream, const bool value)
{
	stream << static_cast<quint8>(value);
}

bool readBool(QDataStream& stream, bool& value)
{
	uint8_t serialized = 0;
	if (!readByte(stream, serialized) || serialized > 1)
		return false;

	value = serialized != 0;
	return true;
}

void writeNativeString(QDataStream& stream, const NativePath& value)
{
	stream << static_cast<quint32>(value.size());
#ifdef _WIN32
	for (const QChar character : value)
		stream << static_cast<quint16>(character.unicode());
#else
	stream.writeRawData(value.constData(), static_cast<int>(value.size()));
#endif
}

bool readNativeString(QDataStream& stream, NativePath& value)
{
	uint32_t length = 0;
	quint32 serializedLength = 0;
	stream >> serializedLength;
	length = serializedLength;
	if (stream.status() != QDataStream::Ok || length > MaximumNativeStringLength)
		return false;

	value.resize(static_cast<qsizetype>(length));
#ifdef _WIN32
	for (uint32_t i = 0; i < length; ++i)
	{
		quint16 character = 0;
		stream >> character;
		if (stream.status() != QDataStream::Ok)
			return false;
		value[static_cast<qsizetype>(i)] = QChar{character};
	}
	return true;
#else
	return stream.readRawData(value.data(), static_cast<int>(length)) == static_cast<int>(length);
#endif
}

void writeAttributes(QDataStream& stream, const thin_io::entry_attributes& attributes)
{
	writeEnum(stream, attributes.kind);
	writeBool(stream, attributes.is_link);
	writeBool(stream, attributes.sparse);
	writeBool(stream, attributes.compressed);
	stream << static_cast<quint32>(attributes.reparse_tag);
}

bool readAttributes(QDataStream& stream, thin_io::entry_attributes& attributes)
{
	uint8_t kind = 0;
	if (!readByte(stream, kind) || kind > static_cast<uint8_t>(thin_io::entry_kind::other))
		return false;

	attributes.kind = static_cast<thin_io::entry_kind>(kind);
	if (!readBool(stream, attributes.is_link) || !readBool(stream, attributes.sparse) || !readBool(stream, attributes.compressed))
		return false;

	quint32 reparseTag = 0;
	stream >> reparseTag;
	attributes.reparse_tag = reparseTag;
	return stream.status() == QDataStream::Ok;
}

void writeIdentity(QDataStream& stream, const thin_io::entry_identity& identity)
{
	writeUint64(stream, identity.filesystem);
	for (const uint8_t byte : identity.entry)
		stream << static_cast<quint8>(byte);
}

bool readIdentity(QDataStream& stream, thin_io::entry_identity& identity)
{
	if (!readUint64(stream, identity.filesystem))
		return false;
	for (uint8_t& byte : identity.entry)
	{
		quint8 serialized = 0;
		stream >> serialized;
		byte = serialized;
		if (stream.status() != QDataStream::Ok)
			return false;
	}
	return stream.status() == QDataStream::Ok;
}

void writeOptionalIdentity(QDataStream& stream, const std::optional<thin_io::entry_identity>& identity)
{
	writeBool(stream, identity.has_value());
	if (identity)
		writeIdentity(stream, *identity);
}

bool readOptionalIdentity(QDataStream& stream, std::optional<thin_io::entry_identity>& identity)
{
	bool hasIdentity = false;
	if (!readBool(stream, hasIdentity))
		return false;

	if (!hasIdentity)
	{
		identity.reset();
		return true;
	}

	thin_io::entry_identity value;
	if (!readIdentity(stream, value))
		return false;
	identity = value;
	return true;
}

void writeEntryMetadata(QDataStream& stream, const SnapshotEntryMetadata& metadata)
{
	writeUint64(stream, metadata.logicalSize);
	writeUint64(stream, metadata.allocatedSize);
	writeUint64(stream, metadata.hardLinkCount);
	writeOptionalIdentity(stream, metadata.identity);
}

bool readEntryMetadata(QDataStream& stream, SnapshotEntryMetadata& metadata)
{
	return readUint64(stream, metadata.logicalSize)
		&& readUint64(stream, metadata.allocatedSize)
		&& readUint64(stream, metadata.hardLinkCount)
		&& readOptionalIdentity(stream, metadata.identity);
}

void writeOptionalEntryMetadata(QDataStream& stream, const std::optional<SnapshotEntryMetadata>& metadata)
{
	writeBool(stream, metadata.has_value());
	if (metadata)
		writeEntryMetadata(stream, *metadata);
}

bool readOptionalEntryMetadata(QDataStream& stream, std::optional<SnapshotEntryMetadata>& metadata)
{
	bool hasMetadata = false;
	if (!readBool(stream, hasMetadata))
		return false;

	if (!hasMetadata)
	{
		metadata.reset();
		return true;
	}

	SnapshotEntryMetadata value;
	if (!readEntryMetadata(stream, value))
		return false;
	metadata = value;
	return true;
}

bool isValidNativeName(const NativeName& name)
{
	if (name.isEmpty() || name.size() > MaximumNativeStringLength)
		return false;
#ifdef _WIN32
	return name != "." && name != ".." && !name.contains(QChar{}) && !name.contains('/') && !name.contains('\\');
#else
	return name != "." && name != ".." && !name.contains('\0') && !name.contains('/');
#endif
}

bool isValidRootPath(const NativePath& path)
{
	return path.size() <= MaximumNativeStringLength && isAbsoluteNativePath(path);
}

bool isValidEntry(const SnapshotEntry& entry, const uint32_t depth, uint64_t& totalEntryCount)
{
	if (depth > MaximumTreeDepth || ++totalEntryCount > MaximumEntryCount)
		return false;

	const auto kind = entry.attributes.kind;
	if (kind > thin_io::entry_kind::other)
		return false;

	if (!entry.attributes.is_link && entry.attributes.reparse_tag != 0)
		return false;
	if (entry.metadata && entry.metadata->hardLinkCount == 0)
		return false;

	if (kind != thin_io::entry_kind::directory)
		return entry.traversalState == DirectoryTraversalState::not_directory && entry.children.empty();

	switch (entry.traversalState)
	{
	case DirectoryTraversalState::completed:
		if (!entry.metadata || entry.attributes.is_link)
			return false;
		break;
	case DirectoryTraversalState::enumeration_failed:
		if (!entry.metadata || entry.attributes.is_link || !entry.children.empty())
			return false;
		break;
	case DirectoryTraversalState::metadata_unavailable:
		if (entry.metadata || !entry.children.empty())
			return false;
		break;
	case DirectoryTraversalState::link_boundary:
		if (!entry.attributes.is_link || !entry.children.empty())
			return false;
		break;
	case DirectoryTraversalState::mount_boundary:
		if (!entry.metadata || entry.attributes.is_link || !entry.metadata->identity || !entry.children.empty())
			return false;
		break;
	case DirectoryTraversalState::not_directory:
		return false;
	}

	for (const auto& [name, child] : entry.children)
	{
		if (!isValidNativeName(name) || !isValidEntry(child, depth + 1, totalEntryCount))
			return false;
	}
	return true;
}

bool isValidSpace(const thin_io::filesystem_space& space)
{
	return space.available <= space.free && space.available <= space.capacity;
}

bool identitiesAgree(const Snapshot& snapshot)
{
	std::optional<thin_io::filesystem_identity> expected;
	if (snapshot.root.metadata && snapshot.root.metadata->identity)
		expected = snapshot.root.metadata->identity->filesystem;

	for (const auto* space : {snapshot.filesystemSpaceAtStart ? &*snapshot.filesystemSpaceAtStart : nullptr,
		 snapshot.filesystemSpaceAtCompletion ? &*snapshot.filesystemSpaceAtCompletion : nullptr})
	{
		if (!space || !space->identity)
			continue;
		if (expected && *expected != *space->identity)
			return false;
		expected = space->identity;
	}
	return true;
}

bool isValidSnapshot(const Snapshot& snapshot)
{
	if (!isValidRootPath(snapshot.rootPath)
		|| snapshot.root.attributes.kind != thin_io::entry_kind::directory
		|| snapshot.root.attributes.is_link
		|| !snapshot.root.metadata
		|| snapshot.root.traversalState != DirectoryTraversalState::completed
		|| !snapshot.scanStartedAtUtc.isValid()
		|| !snapshot.scanCompletedAtUtc.isValid()
		|| snapshot.scanStartedAtUtc.timeSpec() != Qt::UTC
		|| snapshot.scanCompletedAtUtc.timeSpec() != Qt::UTC
		|| snapshot.scanStartedAtUtc > snapshot.scanCompletedAtUtc
		|| snapshot.diagnostics.size() > MaximumDiagnosticCount
		|| (snapshot.filesystemSpaceAtStart && !isValidSpace(*snapshot.filesystemSpaceAtStart))
		|| (snapshot.filesystemSpaceAtCompletion && !isValidSpace(*snapshot.filesystemSpaceAtCompletion))
		|| !identitiesAgree(snapshot))
		return false;

	uint64_t totalEntryCount = 0;
	if (!isValidEntry(snapshot.root, 0, totalEntryCount))
		return false;

	for (const SnapshotDiagnostic& diagnostic : snapshot.diagnostics)
	{
		if (!isValidRootPath(diagnostic.path)
			|| diagnostic.operation > SnapshotOperation::entry_changed_during_scan
			|| ((diagnostic.operation == SnapshotOperation::entry_changed_during_scan) == diagnostic.nativeErrorCode.has_value()))
			return false;
	}
	return true;
}

void writeEntry(QDataStream& stream, const SnapshotEntry& entry)
{
	writeAttributes(stream, entry.attributes);
	writeOptionalEntryMetadata(stream, entry.metadata);
	writeEnum(stream, entry.traversalState);
	stream << static_cast<quint32>(entry.children.size());
	for (const auto& [name, child] : entry.children)
	{
		writeNativeString(stream, name);
		writeEntry(stream, child);
	}
}

bool readEntry(QDataStream& stream, SnapshotEntry& entry, const uint32_t depth, uint64_t& totalEntryCount)
{
	if (depth > MaximumTreeDepth || ++totalEntryCount > MaximumEntryCount)
		return false;

	uint8_t traversalState = 0;
	uint32_t childCount = 0;
	if (!readAttributes(stream, entry.attributes)
		|| !readOptionalEntryMetadata(stream, entry.metadata)
		|| !readByte(stream, traversalState)
		|| traversalState > static_cast<uint8_t>(DirectoryTraversalState::mount_boundary))
		return false;

	entry.traversalState = static_cast<DirectoryTraversalState>(traversalState);
	quint32 serializedChildCount = 0;
	stream >> serializedChildCount;
	childCount = serializedChildCount;
	if (stream.status() != QDataStream::Ok || childCount > MaximumEntryCount - totalEntryCount)
		return false;

	entry.children.clear();
	for (uint32_t i = 0; i < childCount; ++i)
	{
		NativeName name;
		SnapshotEntry child;
		if (!readNativeString(stream, name) || !isValidNativeName(name) || !readEntry(stream, child, depth + 1, totalEntryCount))
			return false;
		if (!entry.children.emplace(std::move(name), std::move(child)).second)
			return false;
	}
	return true;
}

void writeFilesystemSpace(QDataStream& stream, const thin_io::filesystem_space& space)
{
	writeUint64(stream, space.capacity);
	writeUint64(stream, space.free);
	writeUint64(stream, space.available);
	writeBool(stream, space.identity.has_value());
	if (space.identity)
		writeUint64(stream, *space.identity);
}

bool readFilesystemSpace(QDataStream& stream, thin_io::filesystem_space& space)
{
	if (!readUint64(stream, space.capacity) || !readUint64(stream, space.free) || !readUint64(stream, space.available))
		return false;
	bool hasIdentity = false;
	if (!readBool(stream, hasIdentity))
		return false;
	if (hasIdentity)
	{
		uint64_t identity = 0;
		if (!readUint64(stream, identity))
			return false;
		space.identity = identity;
	}
	else
	{
		space.identity.reset();
	}
	return stream.status() == QDataStream::Ok;
}

void writeOptionalFilesystemSpace(QDataStream& stream, const std::optional<thin_io::filesystem_space>& space)
{
	writeBool(stream, space.has_value());
	if (space)
		writeFilesystemSpace(stream, *space);
}

bool readOptionalFilesystemSpace(QDataStream& stream, std::optional<thin_io::filesystem_space>& space)
{
	bool hasSpace = false;
	if (!readBool(stream, hasSpace))
		return false;
	if (!hasSpace)
	{
		space.reset();
		return true;
	}

	thin_io::filesystem_space value;
	if (!readFilesystemSpace(stream, value))
		return false;
	space = value;
	return true;
}

QByteArray serializePayload(const Snapshot& snapshot)
{
	QByteArray payload;
	QDataStream stream{&payload, QIODevice::WriteOnly};
	configureStream(stream);

	writeNativeString(stream, snapshot.rootPath);
	writeEntry(stream, snapshot.root);
	writeOptionalFilesystemSpace(stream, snapshot.filesystemSpaceAtStart);
	writeOptionalFilesystemSpace(stream, snapshot.filesystemSpaceAtCompletion);
	stream << static_cast<qint64>(snapshot.scanStartedAtUtc.toMSecsSinceEpoch())
		<< static_cast<qint64>(snapshot.scanCompletedAtUtc.toMSecsSinceEpoch());
	stream << static_cast<quint32>(snapshot.diagnostics.size());
	for (const SnapshotDiagnostic& diagnostic : snapshot.diagnostics)
	{
		writeNativeString(stream, diagnostic.path);
		writeEnum(stream, diagnostic.operation);
		writeBool(stream, diagnostic.nativeErrorCode.has_value());
		if (diagnostic.nativeErrorCode)
			stream << static_cast<qint64>(*diagnostic.nativeErrorCode);
	}

	return stream.status() == QDataStream::Ok ? payload : QByteArray{};
}

enum class PayloadReadResult {
	success,
	truncated,
	corrupt,
	trailing
};

PayloadReadResult deserializePayload(const QByteArray& payload, Snapshot& snapshot)
{
	QDataStream stream{payload};
	configureStream(stream);

	uint64_t totalEntryCount = 0;
	qint64 startedAt = 0;
	qint64 completedAt = 0;
	uint32_t diagnosticCount = 0;
	if (!readNativeString(stream, snapshot.rootPath)
		|| !readEntry(stream, snapshot.root, 0, totalEntryCount)
		|| !readOptionalFilesystemSpace(stream, snapshot.filesystemSpaceAtStart)
		|| !readOptionalFilesystemSpace(stream, snapshot.filesystemSpaceAtCompletion))
		return stream.status() == QDataStream::ReadPastEnd ? PayloadReadResult::truncated : PayloadReadResult::corrupt;

	quint32 serializedDiagnosticCount = 0;
	stream >> startedAt >> completedAt >> serializedDiagnosticCount;
	diagnosticCount = serializedDiagnosticCount;
	if (stream.status() != QDataStream::Ok)
		return PayloadReadResult::truncated;
	if (diagnosticCount > MaximumDiagnosticCount)
		return PayloadReadResult::corrupt;

	snapshot.scanStartedAtUtc = QDateTime::fromMSecsSinceEpoch(startedAt, QTimeZone::UTC);
	snapshot.scanCompletedAtUtc = QDateTime::fromMSecsSinceEpoch(completedAt, QTimeZone::UTC);
	snapshot.diagnostics.clear();
	snapshot.diagnostics.reserve(diagnosticCount);
	for (uint32_t i = 0; i < diagnosticCount; ++i)
	{
		SnapshotDiagnostic diagnostic;
		uint8_t operation = 0;
		bool hasNativeErrorCode = false;
		qint64 nativeErrorCode = 0;
		if (!readNativeString(stream, diagnostic.path) || !readByte(stream, operation) || !readBool(stream, hasNativeErrorCode))
			return stream.status() == QDataStream::ReadPastEnd ? PayloadReadResult::truncated : PayloadReadResult::corrupt;
		if (hasNativeErrorCode)
		{
			stream >> nativeErrorCode;
			if (stream.status() != QDataStream::Ok)
				return PayloadReadResult::truncated;
		}
		if (operation > static_cast<uint8_t>(SnapshotOperation::entry_changed_during_scan)
			|| (hasNativeErrorCode
				&& (nativeErrorCode < std::numeric_limits<thin_io::filesystem_error_code>::min()
					|| nativeErrorCode > std::numeric_limits<thin_io::filesystem_error_code>::max())))
			return PayloadReadResult::corrupt;

		diagnostic.operation = static_cast<SnapshotOperation>(operation);
		if (hasNativeErrorCode)
			diagnostic.nativeErrorCode = static_cast<thin_io::filesystem_error_code>(nativeErrorCode);
		snapshot.diagnostics.push_back(std::move(diagnostic));
	}

	if (!stream.atEnd())
		return PayloadReadResult::trailing;
	return isValidSnapshot(snapshot) ? PayloadReadResult::success : PayloadReadResult::corrupt;
}

SnapshotLoadError loadError(const SnapshotLoadErrorCode code, QString systemMessage = {})
{
	return {code, std::move(systemMessage)};
}

SnapshotSaveError saveError(const SnapshotSaveErrorCode code, QString systemMessage = {})
{
	return {code, std::move(systemMessage)};
}

bool isKnownPlatform(const uint8_t platform)
{
	return platform >= static_cast<uint8_t>(SnapshotPlatform::windows)
		&& platform <= static_cast<uint8_t>(SnapshotPlatform::freebsd);
}

struct HardLinkEntry
{
	SnapshotEntry* entry;
	NativePath path;
};

using HardLinkEntries = std::map<thin_io::entry_identity, std::vector<HardLinkEntry>, SnapshotInternal::EntryIdentityLess>;

bool localCoverageIsComplete(const SnapshotEntry& entry)
{
	if (entry.attributes.kind != thin_io::entry_kind::directory)
		return true;

	return entry.traversalState == DirectoryTraversalState::completed
		|| entry.traversalState == DirectoryTraversalState::link_boundary
		|| entry.traversalState == DirectoryTraversalState::mount_boundary;
}

void initializeDerivedData(SnapshotEntry& entry, const NativePath& path, HardLinkEntries& hardLinkEntries)
{
	entry.derived = {};
	entry.derived.localCoverageComplete = localCoverageIsComplete(entry);

	if (entry.traversalState == DirectoryTraversalState::mount_boundary)
	{
		entry.derived.localAllocatedSize = 0;
	}
	else if (entry.metadata)
	{
		const bool isRegularFile = entry.attributes.kind == thin_io::entry_kind::regular_file;
		if (isRegularFile && entry.metadata->identity)
		{
			hardLinkEntries[*entry.metadata->identity].push_back({&entry, path});
		}
		else if (!isRegularFile || entry.metadata->hardLinkCount == 1)
		{
			entry.derived.localAllocatedSize = entry.metadata->allocatedSize;
		}
	}

	for (auto& [name, child] : entry.children)
		initializeDerivedData(child, appendNativeName(path, name), hardLinkEntries);
}

bool hardLinkMetadataMatches(const SnapshotEntry& left, const SnapshotEntry& right)
{
	return left.attributes == right.attributes
		&& left.metadata->logicalSize == right.metadata->logicalSize
		&& left.metadata->allocatedSize == right.metadata->allocatedSize
		&& left.metadata->hardLinkCount == right.metadata->hardLinkCount;
}

SnapshotHardLinkGroup deriveHardLinkGroup(const thin_io::entry_identity& identity, std::vector<HardLinkEntry>& entries)
{
	std::ranges::sort(entries, [](const HardLinkEntry& left, const HardLinkEntry& right) { return left.path < right.path; });

	SnapshotHardLinkGroup group;
	group.identity = identity;
	group.presentationPath = entries.front().path;
	group.allocatedSize = entries.front().entry->metadata->allocatedSize;
	group.reportedLinkCount = entries.front().entry->metadata->hardLinkCount;
	group.aliases.reserve(entries.size());

	group.metadataConsistent = std::ranges::all_of(entries, [&entries](const HardLinkEntry& candidate) {
		return hardLinkMetadataMatches(*entries.front().entry, *candidate.entry);
	});
	if (entries.size() > group.reportedLinkCount)
		group.metadataConsistent = false;

	group.allAliasesObserved = group.metadataConsistent && entries.size() == group.reportedLinkCount;
	group.accountingExact = group.allAliasesObserved;

	for (HardLinkEntry& hardLinkEntry : entries)
	{
		group.aliases.push_back(hardLinkEntry.path);
		hardLinkEntry.entry->derived.localAllocatedSize = group.metadataConsistent ? std::optional<uint64_t>{0} : std::nullopt;
	}
	entries.front().entry->derived.localAllocatedSize = group.accountingExact
		? std::optional<uint64_t>{group.allocatedSize}
		: std::nullopt;
	return group;
}

void aggregateDerivedData(SnapshotEntry& entry)
{
	entry.derived.subtreeCoverageComplete = entry.derived.localCoverageComplete;
	entry.derived.allocationOverflow = false;
	std::optional<uint64_t> subtreeAllocatedSize = entry.derived.localAllocatedSize;

	for (auto& namedChild : entry.children)
	{
		SnapshotEntry& child = namedChild.second;
		aggregateDerivedData(child);
		entry.derived.subtreeCoverageComplete &= child.derived.subtreeCoverageComplete;
		subtreeAllocatedSize = SnapshotInternal::addAllocatedSizes(
			subtreeAllocatedSize, child.derived.subtreeAllocatedSize, entry.derived.allocationOverflow);
	}

	if (!entry.derived.subtreeCoverageComplete)
		subtreeAllocatedSize.reset();
	entry.derived.subtreeAllocatedSize = subtreeAllocatedSize;
}

} // namespace

SnapshotPlatform currentSnapshotPlatform() noexcept
{
#ifdef _WIN32
	return SnapshotPlatform::windows;
#elif defined(__APPLE__)
	return SnapshotPlatform::macos;
#elif defined(__linux__)
	return SnapshotPlatform::linux_os;
#elif defined(__FreeBSD__)
	return SnapshotPlatform::freebsd;
#else
#error Unsupported platform
#endif
}

std::expected<void, SnapshotSaveError> Snapshot::save(const QString& path) const
{
	if (!isValidSnapshot(*this))
		return std::unexpected{saveError(SnapshotSaveErrorCode::invalid_snapshot)};

	const QByteArray payload = serializePayload(*this);
	if (payload.isEmpty() || payload.size() > std::numeric_limits<int>::max())
		return std::unexpected{saveError(SnapshotSaveErrorCode::serialization_failed)};

	const QByteArray compressedPayload = qCompress(payload, 3);
	if (compressedPayload.isEmpty() || compressedPayload.size() > std::numeric_limits<quint32>::max())
		return std::unexpected{saveError(SnapshotSaveErrorCode::serialization_failed)};

	QByteArray fileData;
	fileData.reserve(FileHeaderSize + compressedPayload.size());
	fileData.append(FileMagic, sizeof(FileMagic));
	{
		QDataStream header{&fileData, QIODevice::Append};
		configureStream(header);
		header << static_cast<quint16>(CurrentFormatVersion);
		writeEnum(header, currentSnapshotPlatform());
		header << static_cast<quint32>(compressedPayload.size());
		if (header.status() != QDataStream::Ok)
			return std::unexpected{saveError(SnapshotSaveErrorCode::serialization_failed)};
	}
	fileData += compressedPayload;

	QSaveFile file{path};
	if (!file.open(QIODevice::WriteOnly))
		return std::unexpected{saveError(SnapshotSaveErrorCode::open_failed, file.errorString())};
	if (file.write(fileData) != fileData.size())
	{
		const QString message = file.errorString();
		file.cancelWriting();
		return std::unexpected{saveError(SnapshotSaveErrorCode::write_failed, message)};
	}
	if (!file.commit())
		return std::unexpected{saveError(SnapshotSaveErrorCode::commit_failed, file.errorString())};
	return {};
}

std::expected<Snapshot, SnapshotLoadError> Snapshot::load(const QString& path)
{
	QFile file{path};
	if (!file.open(QIODevice::ReadOnly))
		return std::unexpected{loadError(SnapshotLoadErrorCode::open_failed, file.errorString())};

	const QByteArray fileData = file.readAll();
	if (file.error() != QFileDevice::NoError)
		return std::unexpected{loadError(SnapshotLoadErrorCode::read_failed, file.errorString())};

	const QByteArray magic{FileMagic, sizeof(FileMagic)};
	if (fileData.size() < magic.size())
	{
		const bool isTruncatedHeader = magic.startsWith(fileData);
		return std::unexpected{loadError(isTruncatedHeader ? SnapshotLoadErrorCode::truncated : SnapshotLoadErrorCode::unsupported_legacy_format)};
	}
	if (!fileData.startsWith(magic))
		return std::unexpected{loadError(SnapshotLoadErrorCode::unsupported_legacy_format)};
	if (fileData.size() < FileHeaderSize)
		return std::unexpected{loadError(SnapshotLoadErrorCode::truncated)};

	QDataStream header{fileData.sliced(sizeof(FileMagic), sizeof(uint16_t) + sizeof(uint8_t) + sizeof(uint32_t))};
	configureStream(header);
	quint16 version = 0;
	uint8_t platform = 0;
	quint32 compressedSize = 0;
	header >> version >> platform >> compressedSize;
	if (header.status() != QDataStream::Ok)
		return std::unexpected{loadError(SnapshotLoadErrorCode::truncated)};
	if (version != CurrentFormatVersion)
		return std::unexpected{loadError(SnapshotLoadErrorCode::unsupported_version)};
	if (!isKnownPlatform(platform))
		return std::unexpected{loadError(SnapshotLoadErrorCode::corrupt_data)};
	if (platform != static_cast<uint8_t>(currentSnapshotPlatform()))
		return std::unexpected{loadError(SnapshotLoadErrorCode::wrong_platform)};
	const qsizetype remainingSize = fileData.size() - FileHeaderSize;
	if (compressedSize > static_cast<quint64>(remainingSize))
		return std::unexpected{loadError(SnapshotLoadErrorCode::truncated)};
	if (compressedSize < static_cast<quint64>(remainingSize))
		return std::unexpected{loadError(SnapshotLoadErrorCode::trailing_data)};

	const QByteArray compressedPayload = fileData.sliced(FileHeaderSize, compressedSize);
	if (compressedPayload.size() < sizeof(uint32_t))
		return std::unexpected{loadError(SnapshotLoadErrorCode::truncated)};
	const quint32 uncompressedSize = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(compressedPayload.constData()));
	if (uncompressedSize == 0 || uncompressedSize > static_cast<quint32>(std::numeric_limits<int>::max()))
		return std::unexpected{loadError(SnapshotLoadErrorCode::decompression_failed)};
	const QByteArray payload = qUncompress(compressedPayload);
	if (payload.isEmpty())
		return std::unexpected{loadError(SnapshotLoadErrorCode::decompression_failed)};

	Snapshot snapshot;
	switch (deserializePayload(payload, snapshot))
	{
	case PayloadReadResult::success:
		snapshot.rebuildDerivedData();
		return snapshot;
	case PayloadReadResult::truncated:
		return std::unexpected{loadError(SnapshotLoadErrorCode::truncated)};
	case PayloadReadResult::corrupt:
		return std::unexpected{loadError(SnapshotLoadErrorCode::corrupt_data)};
	case PayloadReadResult::trailing:
		return std::unexpected{loadError(SnapshotLoadErrorCode::trailing_data)};
	}
	return std::unexpected{loadError(SnapshotLoadErrorCode::corrupt_data)};
}

void Snapshot::rebuildDerivedData()
{
	derivedDataAvailable = false;
	hardLinkGroups.clear();

	HardLinkEntries hardLinkEntries;
	initializeDerivedData(root, rootPath, hardLinkEntries);
	hardLinkGroups.reserve(hardLinkEntries.size());
	for (auto& [identity, entries] : hardLinkEntries)
		hardLinkGroups.push_back(deriveHardLinkGroup(identity, entries));

	aggregateDerivedData(root);
	derivedDataAvailable = true;
}
