#pragma once

#include "native_path.h"

#include "fs.hpp"

#include <vector>

namespace SpaceGuard {

class FilesystemAccess
{
public:
	virtual ~FilesystemAccess() = default;

	[[nodiscard]] virtual thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectory(const NativePath& path) = 0;
	[[nodiscard]] virtual thin_io::filesystem_result<thin_io::entry_metadata> getEntryMetadata(
		const NativePath& path, thin_io::link_behavior linkBehavior) = 0;
	[[nodiscard]] virtual thin_io::filesystem_result<thin_io::filesystem_space> getFilesystemSpace(const NativePath& directoryPath) = 0;
};

class ThinIoFilesystemAccess final : public FilesystemAccess
{
public:
	[[nodiscard]] thin_io::filesystem_result<std::vector<thin_io::directory_entry>> listDirectory(const NativePath& path) override;
	[[nodiscard]] thin_io::filesystem_result<thin_io::entry_metadata> getEntryMetadata(
		const NativePath& path, thin_io::link_behavior linkBehavior) override;
	[[nodiscard]] thin_io::filesystem_result<thin_io::filesystem_space> getFilesystemSpace(const NativePath& directoryPath) override;
};

} // namespace SpaceGuard
