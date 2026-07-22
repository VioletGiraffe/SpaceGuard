#include "native_path.h"

#include <QDir>
#include <QFile>
#include <QUrl>

#include <assert.h>
#include <utility>

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

std::optional<std::vector<NativeName>> nativeDescendantComponents(
	const NativePath& rootPath, const NativePath& path)
{
	if (!isAbsoluteNativePath(rootPath) || !isAbsoluteNativePath(path))
		return {};
	if (path == rootPath)
		return std::vector<NativeName>{};

#ifdef _WIN32
	constexpr QChar separator = '\\';
#else
	constexpr char separator = '/';
#endif
	NativePath descendantPrefix = rootPath;
	if (!descendantPrefix.endsWith(separator))
		descendantPrefix += separator;
	if (!path.startsWith(descendantPrefix))
		return {};

	const NativePath relativePath = path.mid(descendantPrefix.size());
	std::vector<NativeName> components;
	decltype(relativePath.size()) componentStart = 0;
	while (componentStart < relativePath.size())
	{
		auto componentEnd = relativePath.indexOf(separator, componentStart);
		if (componentEnd < 0)
			componentEnd = relativePath.size();
		if (componentEnd == componentStart)
			return {};

		NativeName component = relativePath.mid(componentStart, componentEnd - componentStart);
#ifdef _WIN32
		if (component.contains('/'))
			return {};
#endif
		components.push_back(std::move(component));
		componentStart = componentEnd + 1;
	}
	if (components.empty() || relativePath.endsWith(separator))
		return {};
	return components;
}

QString nativePathForDisplay(const NativePath& path)
{
#ifdef _WIN32
	return path;
#else
	return QFile::decodeName(path);
#endif
}

QByteArray nativePathFileUrl(const NativePath& path)
{
#ifdef _WIN32
	return QUrl::fromLocalFile(path).toEncoded(QUrl::FullyEncoded);
#else
	static constexpr char HexDigits[] = "0123456789ABCDEF";
	QByteArray encoded{"file://"};
	encoded.reserve(encoded.size() + path.size() * 3);
	for (const char character : path)
	{
		const auto byte = static_cast<unsigned char>(character);
		if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9')
			|| byte == '-' || byte == '.' || byte == '_' || byte == '~' || byte == '/')
		{
			encoded += character;
		}
		else
		{
			encoded += '%';
			encoded += HexDigits[byte >> 4];
			encoded += HexDigits[byte & 0x0F];
		}
	}
	return encoded;
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
