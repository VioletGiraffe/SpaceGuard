#include "3rdparty/catch2/catch.hpp"

#include "filesystem_access.h"

#include <QFile>
#include <QTemporaryDir>

#include <algorithm>

TEST_CASE("FilesystemAccess forwards native filesystem operations", "[filesystem-access][integration]")
{
	QTemporaryDir directory;
	REQUIRE(directory.isValid());

	QFile file{directory.filePath("entry.bin")};
	REQUIRE(file.open(QIODevice::WriteOnly));
	REQUIRE(file.write("data") == 4);
	file.close();

	const auto nativeDirectory = normalizedAbsoluteNativePath(directory.path());
	REQUIRE(nativeDirectory);

	const auto entries = FilesystemAccess::listDirectory(*nativeDirectory);
	REQUIRE(entries);

	const auto entry = std::ranges::find_if(*entries, [](const thin_io::directory_entry& candidate) {
#ifdef _WIN32
		return candidate.name == L"entry.bin";
#else
		return candidate.name == "entry.bin";
#endif
	});
	REQUIRE(entry != entries->end());

	const NativePath nativeFile = appendNativeName(*nativeDirectory, nativeNameFromThinIo(entry->name));
	const auto metadata = FilesystemAccess::getEntryMetadata(nativeFile, thin_io::link_behavior::do_not_follow);
	REQUIRE(metadata);
	CHECK(metadata->attributes.kind == thin_io::entry_kind::regular_file);
	CHECK(metadata->logical_size == 4);

	const auto space = FilesystemAccess::getFilesystemSpace(*nativeDirectory);
	REQUIRE(space);
	CHECK(space->capacity > 0);
}
