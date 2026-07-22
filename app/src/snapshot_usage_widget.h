#pragma once

#include "snapshot.h"

#include <QWidget>

#include <map>
#include <memory>

class QTreeWidgetItem;

namespace Ui {
class SnapshotUsageWidget;
}

class SnapshotUsageWidget final : public QWidget
{
	Q_OBJECT

public:
	explicit SnapshotUsageWidget(QWidget* parent = nullptr);
	~SnapshotUsageWidget();

	void setSnapshot(std::shared_ptr<const Snapshot> snapshot);
	void clearSnapshot();

signals:
	void pathActivated(const NativePath& path);

private:
	struct HardLinkPresentation
	{
		NativePath presentationPath;
		bool accountingExact;
	};

	void populateChildren(QTreeWidgetItem* item);

	std::unique_ptr<Ui::SnapshotUsageWidget> m_ui;
	std::shared_ptr<const Snapshot> m_snapshot;
	std::map<NativePath, HardLinkPresentation> m_hardLinkPresentationByAlias;
};
