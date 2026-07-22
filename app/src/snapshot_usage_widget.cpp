#include "snapshot_usage_widget.h"

#include "ui_format.h"
#include "ui_snapshot_usage_widget.h"

#include <QHeaderView>
#include <QLineEdit>
#include <QPushButton>
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

struct DisplayedAllocation
{
	std::optional<uint64_t> bytes;
	bool exact = false;
	bool overflow = false;
};

std::optional<uint64_t> exactDisplayedAllocatedSize(const SnapshotEntry& entry)
{
	if (entry.attributes.kind == thin_io::entry_kind::directory)
		return entry.derived.subtreeAllocatedSize;
	return entry.derived.localAllocatedSize;
}

DisplayedAllocation displayedAllocation(const SnapshotEntry& entry)
{
	if (const std::optional<uint64_t> exactSize = exactDisplayedAllocatedSize(entry))
		return {*exactSize, true, false};
	if (entry.derived.allocationOverflow)
		return {{}, false, true};
	return {entry.derived.knownSubtreeAllocatedSizeLowerBound, false, false};
}

QString formatDisplayedAllocation(const DisplayedAllocation& allocation)
{
	if (allocation.overflow)
		return "Overflow";
	if (!allocation.bytes)
		return "Unknown";
	const QString formatted = formatByteCount(*allocation.bytes);
	return allocation.exact ? formatted : QString{QChar{0x2265}} + " " + formatted;
}

QString formatPercentage(const DisplayedAllocation& numerator, const DisplayedAllocation& denominator)
{
	if (!numerator.bytes || !denominator.bytes || *denominator.bytes == 0)
		return "-";
	return QString::number(
		static_cast<double>(*numerator.bytes) * 100.0 / static_cast<double>(*denominator.bytes), 'f', 1) + "%";
}

QString entryStateSuffix(const SnapshotEntry& entry)
{
	switch (entry.traversalState)
	{
	case DirectoryTraversalState::enumeration_failed:
	case DirectoryTraversalState::metadata_unavailable:
		return {};
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
	const DisplayedAllocation allocation = displayedAllocation(entry);
	if (allocation.overflow)
		qualifications.push_back("Allocated-size arithmetic overflowed; the subtree total is unavailable.");
	else if (!allocation.exact && allocation.bytes)
		qualifications.push_back("The complete total is unavailable; the displayed value includes known allocated space only.");
	else if (!allocation.exact)
		qualifications.push_back("No allocated-size contribution is known.");
	return qualifications.join(' ');
}

void setItemToolTip(QTreeWidgetItem& item, const QString& toolTip)
{
	if (toolTip.isEmpty())
		return;
	for (int column = NameColumn; column <= RootPercentageColumn; ++column)
		item.setToolTip(column, toolTip);
}

SnapshotUsageTreeItem* usageTreeItem(QTreeWidgetItem* item)
{
	auto* usageItem = dynamic_cast<SnapshotUsageTreeItem*>(item);
	assert(usageItem);
	return usageItem;
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
		"Share of the parent's allocated total. When accounting is incomplete, the percentage uses known allocation only. "
		"Visible children may not total 100% because a directory can occupy space itself.");
	m_ui->usageTree->headerItem()->setToolTip(RootPercentageColumn,
		"Share of the scanned-root allocated total. When accounting is incomplete, the percentage uses known allocation only.");

	connect(m_ui->usageTree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { populateChildren(item); });
	connect(m_ui->usageTree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) {
		emit pathActivated(usageTreeItem(item)->path);
	});
	connect(m_ui->usageTree, &QTreeWidget::itemSelectionChanged, this, [this] {
		m_ui->revealSelectedButton->setEnabled(!m_ui->usageTree->selectedItems().isEmpty());
	});
	connect(m_ui->revealSelectedButton, &QAbstractButton::clicked, this, [this] {
		if (QTreeWidgetItem* item = m_ui->usageTree->currentItem())
			emit pathActivated(usageTreeItem(item)->path);
	});
	connect(m_ui->searchEdit, &QLineEdit::textChanged, this, [this](const QString& query) {
		m_lastSearchPath.reset();
		m_ui->searchStatusLabel->clear();
		m_ui->searchStatusLabel->setToolTip(QString{});
		m_ui->findNextButton->setEnabled(m_snapshot && !query.isEmpty());
	});
	connect(m_ui->searchEdit, &QLineEdit::returnPressed, this, [this] { selectNextSearchResult(); });
	connect(m_ui->findNextButton, &QAbstractButton::clicked, this, [this] { selectNextSearchResult(); });
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

	const DisplayedAllocation rootAllocation = displayedAllocation(m_snapshot->root);
	m_ui->usageTree->headerItem()->setText(ParentPercentageColumn, rootAllocation.exact ? "% parent" : "% known parent");
	m_ui->usageTree->headerItem()->setText(RootPercentageColumn, rootAllocation.exact ? "% root" : "% known root");
	const QString sizeText = formatDisplayedAllocation(rootAllocation);
	m_ui->snapshotContextLabel->setText(QString{"%1    Scanned: %2    Allocated: %3"}
		.arg(nativePathForDisplay(m_snapshot->rootPath)).arg(formatSnapshotTime(m_snapshot->scanCompletedAtUtc)).arg(sizeText));

	QString qualification;
	if (rootAllocation.overflow)
		qualification = "Some allocated-size totals overflowed and are marked Overflow.";
	else if (!rootAllocation.exact)
		qualification = QString{QChar{0x2265}}
			+ " marks a known lower bound where some contents could not be measured; Unknown means no allocation is known. "
				"Percentage columns use those known values.";
	m_ui->snapshotQualificationLabel->setText(qualification);
	m_ui->snapshotQualificationLabel->setVisible(!qualification.isEmpty());

	auto* rootItem = new SnapshotUsageTreeItem{m_snapshot->root, m_snapshot->rootPath};
	rootItem->setText(NameColumn, nativePathForDisplay(m_snapshot->rootPath));
	rootItem->setText(AllocatedColumn, sizeText);
	rootItem->setText(ParentPercentageColumn, "-");
	rootItem->setText(RootPercentageColumn, formatPercentage(rootAllocation, rootAllocation));
	rootItem->setChildIndicatorPolicy(m_snapshot->root.children.empty()
		? QTreeWidgetItem::DontShowIndicator : QTreeWidgetItem::ShowIndicator);
	setItemToolTip(*rootItem, entryQualification(m_snapshot->root));
	m_ui->usageTree->addTopLevelItem(rootItem);
	populateChildren(rootItem);
	rootItem->setExpanded(true);
}

void SnapshotUsageWidget::clearSnapshot()
{
	m_ui->usageTree->clear();
	m_hardLinkPresentationByAlias.clear();
	m_snapshot.reset();
	m_lastSearchPath.reset();
	m_ui->searchEdit->clear();
	m_ui->findNextButton->setEnabled(false);
	m_ui->searchStatusLabel->clear();
	m_ui->searchStatusLabel->setToolTip(QString{});
	m_ui->revealSelectedButton->setEnabled(false);
	m_ui->snapshotContextLabel->setText("No current snapshot available.");
	m_ui->snapshotQualificationLabel->clear();
	m_ui->snapshotQualificationLabel->setVisible(false);
	m_ui->usageTree->headerItem()->setText(ParentPercentageColumn, "% parent");
	m_ui->usageTree->headerItem()->setText(RootPercentageColumn, "% root");
}

bool SnapshotUsageWidget::selectPath(const NativePath& path)
{
	if (!m_snapshot)
		return false;
	const std::optional<std::vector<NativeName>> components = nativeDescendantComponents(m_snapshot->rootPath, path);
	if (!components)
		return false;

	auto* currentItem = usageTreeItem(m_ui->usageTree->topLevelItem(0));
	const SnapshotEntry* currentEntry = &m_snapshot->root;
	for (const NativeName& component : *components)
	{
		const auto factualChild = currentEntry->children.find(component);
		if (factualChild == currentEntry->children.end())
			return false;

		populateChildren(currentItem);
		currentItem->setExpanded(true);
		SnapshotUsageTreeItem* childItem = nullptr;
		for (int index = 0; index < currentItem->childCount(); ++index)
		{
			auto* candidate = usageTreeItem(currentItem->child(index));
			if (candidate->entry == &factualChild.value())
			{
				childItem = candidate;
				break;
			}
		}
		assert(childItem);
		currentItem = childItem;
		currentEntry = &factualChild.value();
	}

	m_ui->usageTree->setCurrentItem(currentItem);
	m_ui->usageTree->scrollToItem(currentItem, QAbstractItemView::PositionAtCenter);
	m_ui->usageTree->setFocus();
	return true;
}

void SnapshotUsageWidget::selectNextSearchResult()
{
	if (!m_snapshot || m_ui->searchEdit->text().isEmpty())
		return;
	const std::optional<NativePath> match = findNextMatchingPath(m_ui->searchEdit->text());
	if (!match)
	{
		m_ui->searchStatusLabel->setText("No matches.");
		m_ui->searchStatusLabel->setToolTip(QString{});
		return;
	}

	m_lastSearchPath = match;
	if (!selectPath(*match))
	{
		m_ui->searchStatusLabel->setText("The matching path could not be selected.");
		m_ui->searchStatusLabel->setToolTip(nativePathForDisplay(*match));
		return;
	}
	m_ui->searchStatusLabel->setText("Match selected.");
	m_ui->searchStatusLabel->setToolTip(nativePathForDisplay(*match));
}

std::optional<NativePath> SnapshotUsageWidget::findNextMatchingPath(const QString& query) const
{
	assert(m_snapshot);
#ifdef _WIN32
	constexpr Qt::CaseSensitivity caseSensitivity = Qt::CaseInsensitive;
#else
	constexpr Qt::CaseSensitivity caseSensitivity = Qt::CaseSensitive;
#endif
	std::optional<NativePath> firstMatch;
	std::optional<NativePath> nextMatch;
	bool passedLastMatch = !m_lastSearchPath;
	auto visit = [&](auto&& self, const SnapshotEntry& entry, const NativePath& path) -> bool {
		const bool matches = nativePathForDisplay(path).contains(query, caseSensitivity);
		if (matches && !firstMatch)
			firstMatch = path;
		if (m_lastSearchPath && path == *m_lastSearchPath)
			passedLastMatch = true;
		else if (matches && passedLastMatch)
		{
			nextMatch = path;
			return true;
		}

		for (const auto& [name, child] : entry.children)
		{
			if (self(self, child, appendNativeName(path, name)))
				return true;
		}
		return false;
	};
	visit(visit, m_snapshot->root, m_snapshot->rootPath);
	return nextMatch ? nextMatch : firstMatch;
}

void SnapshotUsageWidget::populateChildren(QTreeWidgetItem* item)
{
	auto* parentItem = usageTreeItem(item);
	if (parentItem->childrenPopulated)
		return;
	parentItem->childrenPopulated = true;

	struct ChildReference
	{
		const NativeName* name;
		const SnapshotEntry* entry;
		NativePath path;
		DisplayedAllocation allocation;
	};
	std::vector<ChildReference> children;
	children.reserve(parentItem->entry->children.size());
	for (auto child = parentItem->entry->children.begin(); child != parentItem->entry->children.end(); ++child)
	{
		NativePath childPath = appendNativeName(parentItem->path, child.key());
		DisplayedAllocation allocation = displayedAllocation(child.value());
		const auto hardLinkPresentation = m_hardLinkPresentationByAlias.find(childPath);
		if (hardLinkPresentation != m_hardLinkPresentationByAlias.end() && !hardLinkPresentation->second.accountingExact)
			allocation = {};
		children.push_back({&child.key(), &child.value(), std::move(childPath), allocation});
	}
	std::sort(children.begin(), children.end(), [](const ChildReference& left, const ChildReference& right) {
		const DisplayedAllocation& leftAllocation = left.allocation;
		const DisplayedAllocation& rightAllocation = right.allocation;
		if (leftAllocation.overflow != rightAllocation.overflow)
			return leftAllocation.overflow;
		if (leftAllocation.bytes.has_value() != rightAllocation.bytes.has_value())
			return leftAllocation.bytes.has_value();
		if (leftAllocation.bytes && *leftAllocation.bytes != *rightAllocation.bytes)
			return *leftAllocation.bytes > *rightAllocation.bytes;
		return *left.name < *right.name;
	});

	const DisplayedAllocation parentAllocation = displayedAllocation(*parentItem->entry);
	const DisplayedAllocation rootAllocation = displayedAllocation(m_snapshot->root);
	for (ChildReference& child : children)
	{
		auto* childItem = new SnapshotUsageTreeItem{*child.entry, std::move(child.path)};
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
			childItem->setText(AllocatedColumn, formatDisplayedAllocation(child.allocation));
			childItem->setText(ParentPercentageColumn, formatPercentage(child.allocation, parentAllocation));
			childItem->setText(RootPercentageColumn, formatPercentage(child.allocation, rootAllocation));
		}
		setItemToolTip(*childItem, toolTip);
		parentItem->addChild(childItem);
	}
}
