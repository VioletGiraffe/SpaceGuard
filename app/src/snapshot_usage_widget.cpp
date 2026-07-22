#include "snapshot_usage_widget.h"

#include "ui_format.h"
#include "ui_snapshot_usage_widget.h"

#include <QHeaderView>
#include <QStringList>
#include <QTreeWidget>

#include <algorithm>
#include <assert.h>
#include <optional>
#include <utility>
#include <vector>

namespace {

constexpr int NameColumn = 0;
constexpr int AllocatedColumn = 1;
constexpr int ParentPercentageColumn = 2;
constexpr int RootPercentageColumn = 3;

class SnapshotUsageTreeItem final : public QTreeWidgetItem
{
public:
	SnapshotUsageTreeItem(const SnapshotEntry& entry_, NativePath path_)
		: entry{&entry_}, path{std::move(path_)}
	{
	}

	const SnapshotEntry* entry;
	NativePath path;
	bool childrenPopulated = false;
};

std::optional<uint64_t> displayedAllocatedSize(const SnapshotEntry& entry)
{
	if (entry.attributes.kind == thin_io::entry_kind::directory)
		return entry.derived.subtreeAllocatedSize;
	return entry.derived.localAllocatedSize;
}

QString formatPercentage(const std::optional<uint64_t> numerator, const std::optional<uint64_t> denominator)
{
	if (!numerator || !denominator || *denominator == 0)
		return "-";
	return QString::number(static_cast<double>(*numerator) * 100.0 / static_cast<double>(*denominator), 'f', 1) + "%";
}

QString entryStateSuffix(const SnapshotEntry& entry)
{
	switch (entry.traversalState)
	{
	case DirectoryTraversalState::enumeration_failed: return " (incomplete)";
	case DirectoryTraversalState::metadata_unavailable: return " (metadata unavailable)";
	case DirectoryTraversalState::link_boundary: return " (link boundary)";
	case DirectoryTraversalState::mount_boundary: return " (mount boundary)";
	case DirectoryTraversalState::not_directory: return entry.attributes.is_link ? " (link)" : QString{};
	case DirectoryTraversalState::completed: return {};
	}
	return {};
}

QString entryQualification(const SnapshotEntry& entry)
{
	QStringList qualifications;
	switch (entry.traversalState)
	{
	case DirectoryTraversalState::enumeration_failed:
		qualifications.push_back("Directory enumeration failed; this subtree may be incomplete.");
		break;
	case DirectoryTraversalState::metadata_unavailable:
		qualifications.push_back("Directory metadata was unavailable; this subtree was not traversed.");
		break;
	case DirectoryTraversalState::link_boundary:
		qualifications.push_back("This link or reparse target was intentionally not traversed.");
		break;
	case DirectoryTraversalState::mount_boundary:
		qualifications.push_back("This filesystem or mount boundary was intentionally not traversed.");
		break;
	case DirectoryTraversalState::not_directory:
	case DirectoryTraversalState::completed:
		break;
	}
	if (entry.attributes.is_link && entry.traversalState != DirectoryTraversalState::link_boundary)
		qualifications.push_back("This link target was intentionally not traversed.");
	if (entry.derived.allocationOverflow)
		qualifications.push_back("Allocated-size arithmetic overflowed; the subtree total is unavailable.");
	else if (!displayedAllocatedSize(entry))
		qualifications.push_back("Complete allocated-size accounting is unavailable.");
	return qualifications.join(' ');
}

void setItemToolTip(QTreeWidgetItem& item, const QString& toolTip)
{
	if (toolTip.isEmpty())
		return;
	for (int column = NameColumn; column <= RootPercentageColumn; ++column)
		item.setToolTip(column, toolTip);
}

} // namespace

SnapshotUsageWidget::SnapshotUsageWidget(QWidget* parent)
	: QWidget{parent}, m_ui{std::make_unique<Ui::SnapshotUsageWidget>()}
{
	m_ui->setupUi(this);
	m_ui->usageTree->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
	for (int column = AllocatedColumn; column <= RootPercentageColumn; ++column)
		m_ui->usageTree->header()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	m_ui->usageTree->headerItem()->setToolTip(ParentPercentageColumn,
		"Share of the parent's complete allocated total. Visible children may not total 100% because a directory can occupy space itself.");
	m_ui->usageTree->headerItem()->setToolTip(RootPercentageColumn, "Share of the complete scanned-root allocated total.");

	connect(m_ui->usageTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { populateChildren(item); });
	connect(m_ui->usageTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
		auto* usageItem = dynamic_cast<SnapshotUsageTreeItem*>(item);
		assert(usageItem);
		emit pathActivated(usageItem->path);
	});
}

SnapshotUsageWidget::~SnapshotUsageWidget()
{
	clearSnapshot();
}

void SnapshotUsageWidget::setSnapshot(std::shared_ptr<const Snapshot> snapshot)
{
	assert(snapshot);
	assert(snapshot->derivedDataAvailable);
	clearSnapshot();
	m_snapshot = std::move(snapshot);

	for (const SnapshotHardLinkGroup& group : m_snapshot->hardLinkGroups)
	{
		for (const NativePath& alias : group.aliases)
		{
			if (alias != group.presentationPath)
				m_hardLinkPresentationByAlias.emplace(alias, HardLinkPresentation{group.presentationPath, group.accountingExact});
		}
	}

	const std::optional<uint64_t> rootSize = displayedAllocatedSize(m_snapshot->root);
	const QString sizeText = rootSize ? formatByteCount(*rootSize) : "Unknown";
	m_ui->snapshotContextLabel->setText(QString{"%1    Scanned: %2    Allocated: %3"}
		.arg(nativePathForDisplay(m_snapshot->rootPath)).arg(formatSnapshotTime(m_snapshot->scanCompletedAtUtc)).arg(sizeText));

	QStringList qualifications;
	if (!m_snapshot->root.derived.subtreeCoverageComplete)
		qualifications.push_back("Directory coverage is incomplete.");
	if (!m_snapshot->root.derived.subtreeAllocatedSize)
		qualifications.push_back(m_snapshot->root.derived.allocationOverflow
			? "Allocated-size arithmetic overflowed." : "Complete allocated-size accounting is unavailable.");
	if (!m_snapshot->diagnostics.empty())
	{
		const auto issueCount = static_cast<qulonglong>(m_snapshot->diagnostics.size());
		qualifications.push_back(QString{"%1 scan issue%2 recorded."}.arg(issueCount).arg(issueCount == 1 ? "" : "s"));
	}
	m_ui->snapshotQualificationLabel->setText(qualifications.empty() ? "Complete scan and allocated-size accounting." : qualifications.join(' '));

	auto* rootItem = new SnapshotUsageTreeItem{m_snapshot->root, m_snapshot->rootPath};
	rootItem->setText(NameColumn, nativePathForDisplay(m_snapshot->rootPath));
	rootItem->setText(AllocatedColumn, sizeText);
	rootItem->setText(ParentPercentageColumn, "-");
	rootItem->setText(RootPercentageColumn, rootSize && *rootSize != 0 ? "100.0%" : "-");
	rootItem->setChildIndicatorPolicy(m_snapshot->root.children.empty()
		? QTreeWidgetItem::DontShowIndicator : QTreeWidgetItem::ShowIndicator);
	setItemToolTip(*rootItem, entryQualification(m_snapshot->root));
	m_ui->usageTree->addTopLevelItem(rootItem);
}

void SnapshotUsageWidget::clearSnapshot()
{
	m_ui->usageTree->clear();
	m_hardLinkPresentationByAlias.clear();
	m_snapshot.reset();
	m_ui->snapshotContextLabel->setText("No current snapshot available.");
	m_ui->snapshotQualificationLabel->clear();
}

void SnapshotUsageWidget::populateChildren(QTreeWidgetItem* item)
{
	auto* parentItem = dynamic_cast<SnapshotUsageTreeItem*>(item);
	assert(parentItem);
	if (parentItem->childrenPopulated)
		return;
	parentItem->childrenPopulated = true;

	struct ChildReference
	{
		const NativeName* name;
		const SnapshotEntry* entry;
	};
	std::vector<ChildReference> children;
	children.reserve(parentItem->entry->children.size());
	for (auto child = parentItem->entry->children.begin(); child != parentItem->entry->children.end(); ++child)
		children.push_back({&child.key(), &child.value()});
	std::sort(children.begin(), children.end(), [](const ChildReference& left, const ChildReference& right) {
		const std::optional<uint64_t> leftSize = displayedAllocatedSize(*left.entry);
		const std::optional<uint64_t> rightSize = displayedAllocatedSize(*right.entry);
		if (leftSize.has_value() != rightSize.has_value())
			return leftSize.has_value();
		if (leftSize && *leftSize != *rightSize)
			return *leftSize > *rightSize;
		return *left.name < *right.name;
	});

	const std::optional<uint64_t> parentSize = displayedAllocatedSize(*parentItem->entry);
	const std::optional<uint64_t> rootSize = displayedAllocatedSize(m_snapshot->root);
	for (const ChildReference& child : children)
	{
		NativePath childPath = appendNativeName(parentItem->path, *child.name);
		auto* childItem = new SnapshotUsageTreeItem{*child.entry, std::move(childPath)};
		childItem->setText(NameColumn, nativePathForDisplay(*child.name) + entryStateSuffix(*child.entry));
		childItem->setChildIndicatorPolicy(child.entry->children.empty()
			? QTreeWidgetItem::DontShowIndicator : QTreeWidgetItem::ShowIndicator);

		QString toolTip = entryQualification(*child.entry);
		const auto presentation = m_hardLinkPresentationByAlias.find(childItem->path);
		if (presentation != m_hardLinkPresentationByAlias.end())
		{
			const QString presentationPath = nativePathForDisplay(presentation->second.presentationPath);
			const QString hardLinkExplanation = presentation->second.accountingExact
				? "Hard link; allocation counted at " + presentationPath + "."
				: "Hard link; complete allocation accounting is unavailable. Preferred presentation path: " + presentationPath + ".";
			childItem->setText(AllocatedColumn, presentation->second.accountingExact ? "Counted elsewhere" : "Unknown");
			childItem->setText(ParentPercentageColumn, "-");
			childItem->setText(RootPercentageColumn, "-");
			toolTip = toolTip.isEmpty() ? hardLinkExplanation : hardLinkExplanation + ' ' + toolTip;
		}
		else
		{
			const std::optional<uint64_t> childSize = displayedAllocatedSize(*child.entry);
			childItem->setText(AllocatedColumn, childSize ? formatByteCount(*childSize) : "Unknown");
			childItem->setText(ParentPercentageColumn, formatPercentage(childSize, parentSize));
			childItem->setText(RootPercentageColumn, formatPercentage(childSize, rootSize));
		}
		setItemToolTip(*childItem, toolTip);
		parentItem->addChild(childItem);
	}
}
