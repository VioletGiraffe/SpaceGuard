#pragma once

#include "fs.hpp"
#include "native_path.h"

#include <vector>

class FilesystemAccess final
{
public:
	[[nodiscard]] static inline thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectory(const NativePath& path)
	{
		return thin_io::list_directory(nativePathData(path));
	}

	[[nodiscard]] static inline thin_io::filesystem_result<thin_io::entry_metadata> getEntryMetadata(
		const NativePath& path, const thin_io::link_behavior linkBehavior)
	{
		return thin_io::get_entry_metadata(nativePathData(path), linkBehavior);
	}

	[[nodiscard]] static inline thin_io::filesystem_result<thin_io::filesystem_space> getFilesystemSpace(const NativePath& directoryPath)
	{
		return thin_io::get_filesystem_space(nativePathData(directoryPath));
	}
};
