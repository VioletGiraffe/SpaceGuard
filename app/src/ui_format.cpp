#include "ui_format.h"

#include <QLocale>

QString formatByteCount(const uint64_t bytes)
{
	constexpr uint64_t KiB = 1024;
	constexpr uint64_t MiB = KiB * 1024;
	constexpr uint64_t GiB = MiB * 1024;
	constexpr uint64_t TiB = GiB * 1024;

	if (bytes < KiB)
		return QString::number(static_cast<qulonglong>(bytes)) + " B";
	if (bytes < MiB)
		return QString::number(static_cast<double>(bytes) / KiB, 'f', 1) + " KiB";
	if (bytes < GiB)
		return QString::number(static_cast<double>(bytes) / MiB, 'f', 1) + " MiB";
	if (bytes < TiB)
		return QString::number(static_cast<double>(bytes) / GiB, 'f', 1) + " GiB";
	return QString::number(static_cast<double>(bytes) / TiB, 'f', 1) + " TiB";
}

QString formatSnapshotTime(const QDateTime& utcTime)
{
	return QLocale{}.toString(utcTime.toLocalTime(), QLocale::ShortFormat);
}
