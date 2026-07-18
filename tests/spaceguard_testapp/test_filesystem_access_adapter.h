#pragma once

#include "fs.hpp"
#include "native_path.h"

#include <assert.h>
#include <functional>
#include <vector>

class TestFilesystemAccess final
{
public:
	[[nodiscard]] static inline thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectory(const NativePath& path)
	{
		assert(s_listDirectory);
		return s_listDirectory(path);
	}

	[[nodiscard]] static inline thin_io::filesystem_result<thin_io::entry_metadata> getEntryMetadata(
		const NativePath& path, const thin_io::link_behavior linkBehavior)
	{
		assert(s_getEntryMetadata);
		return s_getEntryMetadata(path, linkBehavior);
	}

	[[nodiscard]] static inline thin_io::filesystem_result<thin_io::filesystem_space> getFilesystemSpace(const NativePath& path)
	{
		assert(s_getFilesystemSpace);
		return s_getFilesystemSpace(path);
	}

private:
	template<class Filesystem>
	static void bind(Filesystem& filesystem)
	{
		assert(!s_listDirectory && !s_getEntryMetadata && !s_getFilesystemSpace);
		s_listDirectory = [&filesystem](const NativePath& path) { return filesystem.listDirectory(path); };
		s_getEntryMetadata = [&filesystem](const NativePath& path, const thin_io::link_behavior linkBehavior) {
			return filesystem.getEntryMetadata(path, linkBehavior);
		};
		s_getFilesystemSpace = [&filesystem](const NativePath& path) { return filesystem.getFilesystemSpace(path); };
	}

	static void unbind()
	{
		s_listDirectory = {};
		s_getEntryMetadata = {};
		s_getFilesystemSpace = {};
	}

	inline static std::function<thin_io::filesystem_result<std::vector<thin_io::directory_entry>>(const NativePath&)> s_listDirectory;
	inline static std::function<thin_io::filesystem_result<thin_io::entry_metadata>(const NativePath&, thin_io::link_behavior)> s_getEntryMetadata;
	inline static std::function<thin_io::filesystem_result<thin_io::filesystem_space>(const NativePath&)> s_getFilesystemSpace;

	friend class ScopedTestFilesystemAccess;
};

// Only one binding may be active in the test process. It remains unchanged during a scan, so worker threads may call it concurrently.
class ScopedTestFilesystemAccess final
{
public:
	template<class Filesystem>
	explicit ScopedTestFilesystemAccess(Filesystem& filesystem)
	{
		TestFilesystemAccess::bind(filesystem);
	}

	~ScopedTestFilesystemAccess()
	{
		TestFilesystemAccess::unbind();
	}

	ScopedTestFilesystemAccess(const ScopedTestFilesystemAccess&) = delete;
	ScopedTestFilesystemAccess& operator=(const ScopedTestFilesystemAccess&) = delete;
};
