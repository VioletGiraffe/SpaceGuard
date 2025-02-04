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
	static Snapshot create(const QString& root);
	static QStringList compare(const Snapshot& oldSnapshot, const Snapshot& newSnapshot, qint64 threshold);

	bool save(const QString& path) const;
	bool load(const QString& path);

	FileSystemItem root;
};
