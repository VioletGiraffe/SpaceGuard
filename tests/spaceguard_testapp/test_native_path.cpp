#include "3rdparty/catch2/catch.hpp"

#include "native_path.h"

#include <string>

using namespace SpaceGuard;

TEST_CASE("Native roots must be absolute and are normalized", "[native-path]")
{
	CHECK_FALSE(normalizedAbsoluteNativePath({}));
	CHECK_FALSE(normalizedAbsoluteNativePath("relative/path"));

#ifdef _WIN32
	const auto path = normalizedAbsoluteNativePath(R"(C:\folder\.\child\..\leaf)");
	REQUIRE(path);
	CHECK(*path == R"(C:\folder\leaf)");
#else
	const auto path = normalizedAbsoluteNativePath("/folder/./child/../leaf");
	REQUIRE(path);
	CHECK(*path == "/folder/leaf");
#endif
}

TEST_CASE("Native child paths are joined before display conversion", "[native-path]")
{
#ifdef _WIN32
	const NativePath root = R"(C:\)";
	const thin_io::native_string listedName = L"data-\u0434\u0430\u043D\u0456";
	const NativeName name = nativeNameFromThinIo(listedName);
	const NativePath child = appendNativeName(root, name);

	CHECK(child == QString::fromWCharArray(L"C:\\data-\u0434\u0430\u043D\u0456"));
	CHECK(std::wstring{nativePathData(child)} == child.toStdWString());
	CHECK(nativePathForDisplay(child) == child);
#else
	thin_io::native_string listedName;
	listedName.push_back(static_cast<char>(0xFF));
	listedName += "-data";

	const NativeName name = nativeNameFromThinIo(listedName);
	const NativePath child = appendNativeName("/root", name);
	const std::string nativeChild{nativePathData(child)};

	REQUIRE(nativeChild.size() == 12);
	CHECK(nativeChild.starts_with("/root/"));
	CHECK(static_cast<unsigned char>(nativeChild[6]) == 0xFF);
	CHECK(nativeChild.substr(7) == "-data");

	const QString displayed = nativePathForDisplay(child);
	CHECK_FALSE(displayed.isEmpty());
	CHECK(std::string{nativePathData(child)} == nativeChild);
#endif
}

#ifdef _WIN32
TEST_CASE("Extended Windows roots remain opaque", "[native-path][windows]")
{
	const QString extended = R"(\\?\C:\folder\..\literal)";
	const auto normalized = normalizedAbsoluteNativePath(extended);

	REQUIRE(normalized);
	CHECK(*normalized == extended);
}
#endif
