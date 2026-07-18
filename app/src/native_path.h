#pragma once

#include "filesystem_types.hpp"

#include <QByteArray>
#include <QString>

#include <optional>

#ifdef _WIN32
using NativePath = QString;
using NativeName = QString;
using NativePathCharacter = wchar_t;
#else
using NativePath = QByteArray;
using NativeName = QByteArray;
using NativePathCharacter = char;
#endif

[[nodiscard]] std::optional<NativePath> normalizedAbsoluteNativePath(const QString& path);
[[nodiscard]] bool isAbsoluteNativePath(const NativePath& path) noexcept;
[[nodiscard]] NativeName nativeNameFromThinIo(const thin_io::native_string& name);
[[nodiscard]] NativePath appendNativeName(const NativePath& parentPath, const NativeName& name);
[[nodiscard]] QString nativePathForDisplay(const NativePath& path);
[[nodiscard]] QByteArray nativePathFileUrl(const NativePath& path);
[[nodiscard]] const NativePathCharacter* nativePathData(const NativePath& path) noexcept;
