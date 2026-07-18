#include "filesystem_access.h"

thin_io::filesystem_result<std::vector<thin_io::directory_entry>> ThinIoFilesystemAccess::listDirectory(const NativePath& path)
{
	return thin_io::list_directory(nativePathData(path));
}

thin_io::filesystem_result<thin_io::entry_metadata> ThinIoFilesystemAccess::getEntryMetadata(
	const NativePath& path, const thin_io::link_behavior linkBehavior)
{
	return thin_io::get_entry_metadata(nativePathData(path), linkBehavior);
}

thin_io::filesystem_result<thin_io::filesystem_space> ThinIoFilesystemAccess::getFilesystemSpace(const NativePath& directoryPath)
{
	return thin_io::get_filesystem_space(nativePathData(directoryPath));
}
