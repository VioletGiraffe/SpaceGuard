#pragma once

#include <QHash>
#include <QStringList>

struct FileSystemItem
{
	QHash<QString, FileSystemItem> children;
	qint64 totalSize = 0;
};

struct Snapshot
{
	[[nodiscard]] static Snapshot create(const QString& root);
	[[nodiscard]] static QStringList compare(const Snapshot& oldSnapshot, const Snapshot& newSnapshot, qint64 threshold);

	[[nodiscard]] bool save(const QString& path) const;
	[[nodiscard]] bool load(const QString& path);

	[[nodiscard]] inline QString path() const { return rootPath; }

private:
	FileSystemItem root;
	QString rootPath;
};
