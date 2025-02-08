#include "snapshot.h"

#include <QBuffer>
#include <QDir>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>

#include <assert.h>

static void buildSnapshot(FileSystemItem& root, const QString& path)
{
	for (const QFileInfo& child : QDir{ path }.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
	{
		if (child.isDir())
		{
			buildSnapshot(root.children[child.fileName() + '/'], child.absoluteFilePath());
		}
		else if (child.isFile())
		{
			root.children[child.fileName()].totalSize = child.size();
		}
	}

	for (const FileSystemItem& node : root.children)
	{
		root.totalSize += node.totalSize;
	}
}

Snapshot Snapshot::create(const QString& root)
{
	assert(root.endsWith('/'));

	Snapshot s;
	s.rootPath = root;
	buildSnapshot(s.root, root);
	return s;
}

static QList<Snapshot::Change> compareFileSystemItems(const FileSystemItem& oldItem, const FileSystemItem& newItem, qint64 threshold, const QString& currentPath)
{
	QList<Snapshot::Change> report;
	const qint64 sizeIncrease = newItem.totalSize - oldItem.totalSize;

	QList<Snapshot::Change> childReports;
	bool hasSignificantChildren = false;

	// 1. Check modified existing items
	for (auto it = oldItem.children.constBegin(); it != oldItem.children.constEnd(); ++it)
	{
		const QString& childName = it.key();
		if (!newItem.children.contains(childName)) continue;

		const FileSystemItem& oldChild = it.value();
		const FileSystemItem& newChild = newItem.children[childName];

		const QString childPath = currentPath + childName;

		QList<Snapshot::Change> subReport = compareFileSystemItems(oldChild, newChild, threshold, childPath);
		if (!subReport.isEmpty())
		{
			childReports += subReport;
			hasSignificantChildren = true;
		}
	}

	// 2. Check new items
	for (auto it = newItem.children.constBegin(); it != newItem.children.constEnd(); ++it)
	{
		const QString& childName = it.key();
		if (oldItem.children.contains(childName)) continue;

		const FileSystemItem& newChild = it.value();
		const qint64 newItemSize = newChild.totalSize;
		const QString childPath = currentPath + childName;

		if (newItemSize >= threshold)
		{
			QList<Snapshot::Change> subReport = compareFileSystemItems(FileSystemItem(), newChild, threshold, childPath);

			if (subReport.isEmpty())
			{
				childReports.emplace_back(Snapshot::Change{childPath, newItemSize}); // TODO: for some reason required for Apple clang
				hasSignificantChildren = true;
			}
			else
			{
				childReports += subReport;
				hasSignificantChildren = true;
			}
		}
	}

	// 3. Determine if we should report this node
	if (sizeIncrease > 0 && sizeIncrease >= threshold)
	{
		if (!hasSignificantChildren)
		{
			// Report this node if no significant children
			report.emplace_back(Snapshot::Change{currentPath, sizeIncrease});
		}
		else
		{
			// Otherwise report children's results
			report += childReports;
		}
	}
	else
	{
		// Still report children even if parent didn't meet threshold
		report += childReports;
	}

	return report;
}

QList<Snapshot::Change> Snapshot::compare(const Snapshot& oldSnapshot, const Snapshot& newSnapshot, qint64 threshold)
{
	if (oldSnapshot.rootPath != newSnapshot.rootPath)
		return { {"ERROR: different root paths!", 0} };

	return compareFileSystemItems(oldSnapshot.root, newSnapshot.root, threshold, oldSnapshot.rootPath);
}

inline QDataStream& operator<<(QDataStream& stream, const FileSystemItem& item)
{
	stream << item.totalSize;
	stream << item.children;
	return stream;
}

inline QDataStream& operator>>(QDataStream& stream, FileSystemItem& item)
{
	stream >> item.totalSize;
	stream >> item.children;
	return stream;
}

bool Snapshot::save(const QString& path) const
{
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return false;

	QBuffer buffer;
	buffer.open(QIODevice::WriteOnly);

	QDataStream out(&buffer);
	out.setVersion(QDataStream::Qt_5_15);
	out << rootPath;
	out << root;

	const QByteArray data = qCompress(buffer.data(), 3);
	if (file.write(data) != data.size())
		return false;

	file.close();
	return true;
}

bool Snapshot::load(const QString& path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;

	root.children.clear();

	QByteArray data = file.readAll();
	data = qUncompress(data);

	QBuffer buffer;
	buffer.setBuffer(&data);
	buffer.open(QIODevice::ReadOnly);

	QDataStream in(&buffer);
	in.setVersion(QDataStream::Qt_5_15);
	in >> rootPath;
	in >> root;

	file.close();
	return true;
}
