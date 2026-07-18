#include "native_path.h"

#include <QDir>
#include <QFile>

#include <assert.h>

bool isAbsoluteNativePath(const NativePath& path) noexcept
{
	if (path.isEmpty())
		return false;

#ifdef _WIN32
	if (path.contains(QChar{}))
		return false;
	if (path.startsWith(R"(\\?\)"))
		return path.size() > 4;
	return (path.size() >= 3 && path[1] == ':' && (path[2] == '\\' || path[2] == '/'))
		|| (path.size() > 2 && (path.startsWith(R"(\\)") || path.startsWith("//")));
#else
	return !path.contains('\0') && path.startsWith('/');
#endif
}

std::optional<NativePath> normalizedAbsoluteNativePath(const QString& path)
{
	if (path.isEmpty())
		return {};

#ifdef _WIN32
	if (path.startsWith(R"(\\?\)"))
		return path.size() > 4 ? std::optional<NativePath>{path} : std::nullopt;
	if (path.startsWith("//?/"))
		return path.size() > 4 ? std::optional<NativePath>{QDir::toNativeSeparators(path)} : std::nullopt;
#endif

	const QString normalized = QDir::cleanPath(QDir::fromNativeSeparators(path));
	if (!QDir::isAbsolutePath(normalized))
		return {};

#ifdef _WIN32
	return QDir::toNativeSeparators(normalized);
#else
	return QFile::encodeName(normalized);
#endif
}

NativeName nativeNameFromThinIo(const thin_io::native_string& name)
{
#ifdef _WIN32
	return QString::fromStdWString(name);
#else
	return QByteArray::fromStdString(name);
#endif
}

NativePath appendNativeName(const NativePath& parentPath, const NativeName& name)
{
	assert(!parentPath.isEmpty());
	assert(!name.isEmpty());

#ifdef _WIN32
	assert(!name.contains('/') && !name.contains('\\'));
	constexpr QChar separator = '\\';
#else
	assert(!name.contains('/'));
	constexpr char separator = '/';
#endif

	NativePath result = parentPath;
	if (!result.endsWith(separator))
		result += separator;
	result += name;
	return result;
}

QString nativePathForDisplay(const NativePath& path)
{
#ifdef _WIN32
	return path;
#else
	return QFile::decodeName(path);
#endif
}

const NativePathCharacter* nativePathData(const NativePath& path) noexcept
{
#ifdef _WIN32
	static_assert(sizeof(QChar) == sizeof(wchar_t));
	return reinterpret_cast<const wchar_t*>(path.utf16());
#else
	return path.constData();
#endif
}
