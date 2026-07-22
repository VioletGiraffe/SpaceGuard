#include "mainwindow.h"

#include "settings.h"
#include "ui_mainwindow.h"

#include "filesystem_error.hpp"
#include "settings/csettings.h"

#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QTableWidget>
#include <QUrl>

#include <algorithm>
#include <assert.h>
#include <exception>
#include <map>
#include <utility>

namespace {

constexpr auto SnapshotExtension = ".spaceguard";
constexpr uint64_t BytesPerMiB = 1024 * 1024;
constexpr int ByteCountRole = Qt::UserRole + 1;

QString formatBytes(const uint64_t bytes)
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

QTableWidgetItem* createByteCountItem(const uint64_t bytes)
{
	auto* item = new QTableWidgetItem{formatBytes(bytes)};
	item->setData(ByteCountRole, static_cast<qulonglong>(bytes));
	return item;
}

QString formatElapsedTime(const qint64 elapsedMilliseconds)
{
	if (elapsedMilliseconds < 1000)
		return "<1 s";

	const qint64 totalSeconds = elapsedMilliseconds / 1000;
	const qint64 seconds = totalSeconds % 60;
	const qint64 totalMinutes = totalSeconds / 60;
	if (totalMinutes == 0)
		return QString{"%1 s"}.arg(totalSeconds);
	if (totalMinutes < 60)
		return QString{"%1 min %2 s"}.arg(totalMinutes).arg(seconds);
	return QString{"%1 h %2 min %3 s"}.arg(totalMinutes / 60).arg(totalMinutes % 60).arg(seconds);
}

QString formatChange(const std::optional<MagnitudeChange>& change)
{
	if (!change)
		return "Unavailable";

	switch (change->direction)
	{
	case ChangeDirection::unchanged:
		return "No change";
	case ChangeDirection::increase:
		return "+" + formatBytes(change->magnitude);
	case ChangeDirection::decrease:
		return "-" + formatBytes(change->magnitude);
	}
	return "Unavailable";
}

std::optional<MagnitudeChange> invertedChange(std::optional<MagnitudeChange> change)
{
	if (!change)
		return {};
	if (change->direction == ChangeDirection::increase)
		change->direction = ChangeDirection::decrease;
	else if (change->direction == ChangeDirection::decrease)
		change->direction = ChangeDirection::increase;
	return change;
}

QString formatUsageChange(const std::optional<MagnitudeChange>& change)
{
	if (!change)
		return "Unavailable";

	switch (change->direction)
	{
	case ChangeDirection::unchanged: return "No change";
	case ChangeDirection::increase: return formatBytes(change->magnitude) + " more used";
	case ChangeDirection::decrease: return formatBytes(change->magnitude) + " less used";
	}
	return "Unavailable";
}

QString formatSnapshotTime(const QDateTime& utcTime)
{
	return QLocale{}.toString(utcTime.toLocalTime(), QLocale::ShortFormat);
}

QString comparisonContext(const Snapshot& baseline, const Snapshot& current)
{
	return QString{"%1    Baseline: %2 %3 Current: %4"}
		.arg(nativePathForDisplay(current.rootPath))
		.arg(formatSnapshotTime(baseline.scanCompletedAtUtc))
		.arg(QChar{0x2192})
		.arg(formatSnapshotTime(current.scanCompletedAtUtc));
}

QString comparisonHeadline(const SnapshotComparisonResult& comparison, const uint64_t threshold)
{
	const auto locationCount = static_cast<qulonglong>(comparison.changes.size());
	const QString locationText = QString{"%1 location%2"}.arg(locationCount).arg(locationCount == 1 ? "" : "s");
	const std::optional<MagnitudeChange>& treeChange = comparison.summary.allocatedTreeChange;
	if (!comparison.changes.empty())
	{
		if (!treeChange)
			return "Growth found in " + locationText + ", but the complete net change could not be calculated.";

		switch (treeChange->direction)
		{
		case ChangeDirection::increase:
			return "Growth found in " + locationText + ". The scanned tree uses " + formatBytes(treeChange->magnitude) + " more overall.";
		case ChangeDirection::decrease:
			return "Growth found in " + locationText + ". The scanned tree uses " + formatBytes(treeChange->magnitude)
				+ " less overall because deletions and shrinkage outweighed this growth.";
		case ChangeDirection::unchanged:
			return "Growth found in " + locationText + ", offset by equal deletions or shrinkage elsewhere in the scanned tree.";
		}
	}

	const QString thresholdText = threshold == 0 ? "positive growth" : "growth of at least " + formatBytes(threshold);
	if (!treeChange)
		return "No comparable " + thresholdText + " was found. Complete net accounting is unavailable.";

	switch (treeChange->direction)
	{
	case ChangeDirection::increase:
		if (threshold == 0)
			return "The scanned tree uses " + formatBytes(treeChange->magnitude) + " more, but no comparable positive-growth location was available.";
		return "The scanned tree uses " + formatBytes(treeChange->magnitude) + " more, but no location reached the "
			+ formatBytes(threshold) + " display threshold.";
	case ChangeDirection::decrease:
		return "No " + thresholdText + " was found. The scanned tree uses " + formatBytes(treeChange->magnitude) + " less overall.";
	case ChangeDirection::unchanged:
		return "No " + thresholdText + " was found, and total scanned-tree usage is unchanged.";
	}
	return {};
}

QString comparisonChangeType(const ComparisonChange& change)
{
	if (change.currentEntryKind == thin_io::entry_kind::directory)
		return change.baselineEntryExists ? "Folder total" : "New folder total";
	return change.baselineEntryExists ? "Expanded" : "New";
}

QString comparisonDetails(const SnapshotComparisonResult& comparison)
{
	const ComparisonSummary& summary = comparison.summary;
	QStringList lines{
		"Available-space change: " + formatChange(summary.availableSpaceChange),
		"Capacity change: " + formatChange(summary.capacityChange)
	};
	switch (summary.reconciliation)
	{
	case ReconciliationState::exact: lines.push_back("Reconciliation: Exact."); break;
	case ReconciliationState::incomplete:
		lines.push_back("Reconciliation: Incomplete; complete whole-volume versus scanned-tree accounting is unavailable.");
		break;
	case ReconciliationState::overflow: lines.push_back("Reconciliation: Arithmetic overflow; the remainder is unavailable."); break;
	}

	for (const SnapshotComparisonWarning warning : comparison.warnings)
	{
		if (warning == SnapshotComparisonWarning::root_identity_unavailable)
			lines.push_back("The root identity was unavailable, so root replacement could not be verified.");
		else
			lines.push_back("The filesystem identity was unavailable, so filesystem replacement could not be verified.");
	}
	if (summary.baselineScanFreeSpaceChange && summary.baselineScanFreeSpaceChange->direction != ChangeDirection::unchanged)
		lines.push_back("Free space changed by " + formatChange(summary.baselineScanFreeSpaceChange) + " while the baseline scan was running.");
	if (summary.currentScanFreeSpaceChange && summary.currentScanFreeSpaceChange->direction != ChangeDirection::unchanged)
		lines.push_back("Free space changed by " + formatChange(summary.currentScanFreeSpaceChange) + " while the current scan was running.");
	return lines.join('\n');
}

QString saveErrorDescription(const SnapshotSaveError& error)
{
	QString description;
	switch (error.code)
	{
	case SnapshotSaveErrorCode::invalid_snapshot: description = "The completed scan is not a valid snapshot."; break;
	case SnapshotSaveErrorCode::serialization_failed: description = "The snapshot could not be serialized."; break;
	case SnapshotSaveErrorCode::open_failed: description = "The destination file could not be opened."; break;
	case SnapshotSaveErrorCode::write_failed: description = "The snapshot could not be written."; break;
	case SnapshotSaveErrorCode::commit_failed: description = "The snapshot file could not be committed atomically."; break;
	}
	if (!error.systemMessage.isEmpty())
		description += "\n\n" + error.systemMessage;
	return description;
}

QString loadErrorDescription(const SnapshotLoadError& error)
{
	QString description;
	switch (error.code)
	{
	case SnapshotLoadErrorCode::open_failed: description = "The snapshot file could not be opened."; break;
	case SnapshotLoadErrorCode::read_failed: description = "The snapshot file could not be read."; break;
	case SnapshotLoadErrorCode::unsupported_legacy_format: description = "Legacy snapshots are not supported."; break;
	case SnapshotLoadErrorCode::unsupported_version: description = "This snapshot version is not supported."; break;
	case SnapshotLoadErrorCode::wrong_platform: description = "The snapshot was created on a different platform."; break;
	case SnapshotLoadErrorCode::decompression_failed: description = "The snapshot data could not be decompressed."; break;
	case SnapshotLoadErrorCode::truncated: description = "The snapshot file is truncated."; break;
	case SnapshotLoadErrorCode::corrupt_data: description = "The snapshot data is corrupt."; break;
	case SnapshotLoadErrorCode::trailing_data: description = "The snapshot contains unexpected trailing data."; break;
	}
	if (!error.systemMessage.isEmpty())
		description += "\n\n" + error.systemMessage;
	return description;
}

QString scanFailureDescription(const SnapshotScanFailure& failure)
{
	QString description;
	switch (failure.code)
	{
	case SnapshotScanFailureCode::invalid_root: description = "The selected root path is invalid."; break;
	case SnapshotScanFailureCode::root_metadata_unavailable: description = "The root metadata could not be read."; break;
	case SnapshotScanFailureCode::root_not_directory: description = "The selected root is not a directory."; break;
	case SnapshotScanFailureCode::root_is_link: description = "A link or reparse point cannot be used as the scan root."; break;
	case SnapshotScanFailureCode::filesystem_space_at_start_unavailable: description = "Filesystem space could not be read before scanning."; break;
	case SnapshotScanFailureCode::root_filesystem_identity_mismatch: description = "The root does not belong to the filesystem reported for the selected path."; break;
	case SnapshotScanFailureCode::root_enumeration_unavailable: description = "The root directory could not be enumerated."; break;
	case SnapshotScanFailureCode::root_filesystem_identity_changed: description = "The root filesystem identity changed during the scan."; break;
	case SnapshotScanFailureCode::unexpected_error: description = "The scan stopped because of an unexpected error."; break;
	}

	description += "\n\nPath: " + nativePathForDisplay(failure.path);
	if (failure.nativeErrorCode)
	{
		const thin_io::filesystem_error error{*failure.nativeErrorCode};
		description += "\n" + QString::fromLocal8Bit(thin_io::format_filesystem_error(error).c_str());
	}
	return description;
}

QString comparisonErrorDescription(const SnapshotComparisonError error)
{
	switch (error)
	{
	case SnapshotComparisonError::invalid_baseline_root: return "The saved snapshot does not contain a valid, completely enumerated root.";
	case SnapshotComparisonError::invalid_current_root: return "The current scan does not contain a valid, completely enumerated root.";
	case SnapshotComparisonError::different_root_paths: return "The saved snapshot and current scan use different root paths.";
	case SnapshotComparisonError::filesystem_identity_mismatch: return "The selected path now refers to a different filesystem.";
	case SnapshotComparisonError::root_identity_mismatch: return "The selected path now refers to a different root directory.";
	}
	return "The snapshots cannot be compared.";
}

QString diagnosticOperationName(const SnapshotOperation operation)
{
	switch (operation)
	{
	case SnapshotOperation::root_metadata: return "Root metadata";
	case SnapshotOperation::directory_enumeration: return "Directory enumeration";
	case SnapshotOperation::entry_metadata: return "Entry metadata";
	case SnapshotOperation::filesystem_space_at_start: return "Filesystem space at start";
	case SnapshotOperation::filesystem_space_at_completion: return "Filesystem space at completion";
	case SnapshotOperation::entry_changed_during_scan: return "Entry changed during scan";
	}
	return "Unknown";
}

QString scanIssueSummary(const std::vector<SnapshotDiagnostic>& diagnostics)
{
	std::map<SnapshotOperation, uint64_t> issueCounts;
	for (const SnapshotDiagnostic& diagnostic : diagnostics)
		++issueCounts[diagnostic.operation];

	QStringList issueKinds;
	for (const auto& [operation, count] : issueCounts)
		issueKinds.push_back(QString{"%1 (%2)"}.arg(diagnosticOperationName(operation)).arg(static_cast<qulonglong>(count)));
	return issueKinds.join(", ");
}

QString excludedRegionReason(const ComparisonExcludedRegion& region)
{
	QStringList reasons;
	if (region.baselineCoverageIncomplete)
		reasons.push_back("baseline coverage incomplete");
	if (region.baselineAccountingUncertain)
		reasons.push_back("baseline accounting uncertain");
	if (region.currentCoverageIncomplete)
		reasons.push_back("current coverage incomplete");
	if (region.currentAccountingUncertain)
		reasons.push_back("current accounting uncertain");
	return reasons.join(", ");
}

void storeNativePath(QTableWidgetItem& item, const NativePath& path)
{
	item.setData(Qt::UserRole, path);
}

NativePath storedNativePath(const QTableWidgetItem& item)
{
#ifdef _WIN32
	return item.data(Qt::UserRole).toString();
#else
	return item.data(Qt::UserRole).toByteArray();
#endif
}

void setRowPath(QTableWidget& table, const int row, const NativePath& path)
{
	for (int column = 0; column < table.columnCount(); ++column)
		storeNativePath(*table.item(row, column), path);
}

QString nativeErrorCodeText(const std::optional<thin_io::filesystem_error_code>& code)
{
	if (!code)
		return {};
#ifdef _WIN32
	return QString::number(static_cast<qulonglong>(*code));
#else
	return QString::number(static_cast<qlonglong>(*code));
#endif
}

QString nativeErrorDescription(const std::optional<thin_io::filesystem_error_code>& code)
{
	if (!code)
		return "No native error code was available.";
	return QString::fromLocal8Bit(thin_io::format_filesystem_error(thin_io::filesystem_error{*code}).c_str());
}

void appendSnapshotDiagnostics(QTableWidget& table, const QString& source, const Snapshot& snapshot)
{
	for (const SnapshotDiagnostic& diagnostic : snapshot.diagnostics)
	{
		const int row = table.rowCount();
		table.insertRow(row);
		table.setItem(row, 0, new QTableWidgetItem{source});
		table.setItem(row, 1, new QTableWidgetItem{diagnosticOperationName(diagnostic.operation)});
		table.setItem(row, 2, new QTableWidgetItem{nativeErrorCodeText(diagnostic.nativeErrorCode)});
		table.setItem(row, 3, new QTableWidgetItem{nativeErrorDescription(diagnostic.nativeErrorCode)});
		table.setItem(row, 4, new QTableWidgetItem{nativePathForDisplay(diagnostic.path)});
		setRowPath(table, row, diagnostic.path);
	}
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
	: QMainWindow{parent},
	  m_ui{std::make_unique<Ui::MainWindow>()},
	  m_scanRunner{m_publicationQueue, {
		  [this](const uint64_t generation, const SnapshotScanProgress& progress) { updateScanProgress(generation, progress); },
		  [this](const uint64_t generation, const std::shared_ptr<const SnapshotScanResult>& result) { scanCompleted(generation, result); }
	  }}
{
	m_ui->setupUi(this);

	connect(m_ui->chooseRootButton, &QAbstractButton::clicked, this, [this] { chooseRootDirectory(); });
	connect(m_ui->createSnapshotButton, &QAbstractButton::clicked, this, [this] { createSnapshot(); });
	connect(m_ui->compareSnapshotButton, &QAbstractButton::clicked, this, [this] { compareWithSnapshot(); });
	connect(m_ui->cancelScanButton, &QAbstractButton::clicked, this, [this] { cancelScan(); });
	connect(m_ui->thresholdSpinBox, &QSpinBox::valueChanged, this, [this](int) { recalculateComparison(); });
	connect(m_ui->detailsButton, &QAbstractButton::toggled, this, [this](const bool expanded) {
		m_ui->detailsWidget->setVisible(expanded);
		m_ui->detailsButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
	});
	connect(&m_publicationTimer, &QTimer::timeout, this, [this] { m_publicationQueue.exec(); });
	connect(&m_scanElapsedUpdateTimer, &QTimer::timeout, this, [this] {
		assert(m_scanElapsedTimer.isValid());
		m_ui->scanDurationLabel->setText("Elapsed: " + formatElapsedTime(m_scanElapsedTimer.elapsed()));
	});
	connect(m_ui->changesTable, &QTableWidget::itemActivated, this, [this](QTableWidgetItem* item) { openTableItem(item); });
	connect(m_ui->excludedTable, &QTableWidget::itemActivated, this, [this](QTableWidgetItem* item) { openTableItem(item); });
	connect(m_ui->diagnosticsTable, &QTableWidget::itemActivated, this, [this](QTableWidgetItem* item) { openTableItem(item); });

	for (QTableWidget* table : {m_ui->changesTable, m_ui->excludedTable, m_ui->diagnosticsTable})
	{
		table->horizontalHeader()->setStretchLastSection(true);
		table->verticalHeader()->setVisible(false);
	}
	m_ui->changesTable->horizontalHeader()->setStretchLastSection(false);
	m_ui->changesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_ui->changesTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
	for (int column = 2; column < m_ui->changesTable->columnCount(); ++column)
		m_ui->changesTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
	m_ui->excludedTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_ui->diagnosticsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
	m_ui->diagnosticsTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);

	CSettings settings;
	m_ui->rootPathEdit->setText(settings.value(Settings::Path).toString());
	m_ui->thresholdSpinBox->setValue(settings.value(Settings::Threshold, 1024).toInt());
	m_ui->thresholdSpinBox->setEnabled(false);
	m_scanElapsedUpdateTimer.setInterval(1000);
	m_publicationTimer.start(33);
}

MainWindow::~MainWindow()
{
	m_publicationTimer.stop();
	m_scanElapsedUpdateTimer.stop();
	CSettings settings;
	settings.setValue(Settings::Path, m_ui->rootPathEdit->text());
	settings.setValue(Settings::Threshold, m_ui->thresholdSpinBox->value());
}

void MainWindow::chooseRootDirectory()
{
	const QString selectedPath = QFileDialog::getExistingDirectory(this, "Select filesystem root", m_ui->rootPathEdit->text());
	if (!selectedPath.isEmpty())
		m_ui->rootPathEdit->setText(selectedPath);
}

void MainWindow::createSnapshot()
{
	const std::optional<NativePath> rootPath = normalizedAbsoluteNativePath(m_ui->rootPathEdit->text());
	if (!rootPath)
	{
		QMessageBox::warning(this, "Invalid root", "Select a valid absolute directory path.");
		return;
	}

	m_baselineSnapshot.reset();
	m_currentSnapshot.reset();
	clearComparisonDisplay();
	beginScan(ScanPurpose::create_snapshot, *rootPath);
}

void MainWindow::compareWithSnapshot()
{
	CSettings settings;
	const QString lastUsedPath = settings.value(Settings::SavePath, QDir::currentPath()).toString();
	const QString snapshotPath = QFileDialog::getOpenFileName(
		this, "Choose baseline snapshot", lastUsedPath, "SpaceGuard snapshots (*.spaceguard)");
	if (snapshotPath.isEmpty())
		return;

	auto loaded = Snapshot::load(snapshotPath);
	if (!loaded)
	{
		QMessageBox::critical(this, "Cannot load snapshot", loadErrorDescription(loaded.error()));
		return;
	}

	settings.setValue(Settings::SavePath, QFileInfo{snapshotPath}.absolutePath());
	m_baselineSnapshot = std::make_shared<const Snapshot>(std::move(*loaded));
	m_currentSnapshot.reset();
	m_ui->rootPathEdit->setText(nativePathForDisplay(m_baselineSnapshot->rootPath));
	clearComparisonDisplay();
	populateDiagnostics();
	beginScan(ScanPurpose::compare_with_snapshot, m_baselineSnapshot->rootPath);
}

void MainWindow::cancelScan()
{
	if (!m_scanRunner.cancel())
		return;
	m_ui->cancelScanButton->setEnabled(false);
	m_ui->scanStatusLabel->setText("Canceling...");
}

void MainWindow::beginScan(const ScanPurpose purpose, const NativePath& rootPath)
{
	try
	{
		const std::optional<uint64_t> generation = m_scanRunner.start(rootPath);
		if (!generation)
		{
			QMessageBox::warning(this, "Scan already active", "Wait for the current scan to finish or cancel it first.");
			return;
		}
		m_activePurpose = purpose;
		m_activeGeneration = *generation;
		m_scanElapsedTimer.start();
		m_scanElapsedUpdateTimer.start();
		m_ui->scanStatusLabel->setText(purpose == ScanPurpose::create_snapshot ? "Creating baseline..." : "Scanning current state...");
		m_ui->scanCountsLabel->clear();
		m_ui->scanDurationLabel->setText("Elapsed: <1 s");
		setScanActive(true);
	}
	catch (const std::exception& error)
	{
		QMessageBox::critical(this, "Cannot start scan", QString::fromLocal8Bit(error.what()));
	}
	catch (...)
	{
		QMessageBox::critical(this, "Cannot start scan", "The background scan could not be started.");
	}
}

void MainWindow::updateScanProgress(const uint64_t generation, const SnapshotScanProgress& progress)
{
	if (!m_activeGeneration || generation != *m_activeGeneration)
		return;
	m_ui->scanCountsLabel->setText(QString{"%1 directories, %2 entries, %3 issues"}
		.arg(static_cast<qulonglong>(progress.directoriesCompleted))
		.arg(static_cast<qulonglong>(progress.entriesDiscovered))
		.arg(static_cast<qulonglong>(progress.issues)));
}

void MainWindow::scanCompleted(
	const uint64_t generation, const std::shared_ptr<const SnapshotScanResult>& result)
{
	if (!m_activeGeneration || generation != *m_activeGeneration)
		return;

	const ScanPurpose purpose = *m_activePurpose;
	m_scanElapsedUpdateTimer.stop();
	assert(m_scanElapsedTimer.isValid());
	const QString elapsedTime = formatElapsedTime(m_scanElapsedTimer.elapsed());
	m_scanElapsedTimer.invalidate();
	m_activeGeneration.reset();
	m_activePurpose.reset();
	setScanActive(false);

	if (std::holds_alternative<SnapshotScanCanceled>(*result))
	{
		m_ui->scanStatusLabel->setText("Scan canceled.");
		m_ui->scanDurationLabel->setText("Stopped after: " + elapsedTime);
		return;
	}
	if (const auto* failure = std::get_if<SnapshotScanFailure>(result.get()))
	{
		m_ui->scanStatusLabel->setText("Scan failed.");
		m_ui->scanDurationLabel->setText("Stopped after: " + elapsedTime);
		QMessageBox::critical(this, "Scan failed", scanFailureDescription(*failure));
		return;
	}

	const Snapshot& snapshot = std::get<Snapshot>(*result);
	const std::shared_ptr<const Snapshot> completedSnapshot{result, &snapshot};
	if (snapshot.diagnostics.empty())
		m_ui->scanStatusLabel->setText("Scan complete: no issues.");
	else
	{
		const auto issueCount = static_cast<qulonglong>(snapshot.diagnostics.size());
		m_ui->scanStatusLabel->setText(QString{"Scan complete: %1 issue%2 - %3."}
			.arg(issueCount).arg(issueCount == 1 ? "" : "s").arg(scanIssueSummary(snapshot.diagnostics)));
	}
	m_ui->scanDurationLabel->setText("Scan time: " + elapsedTime);
	if (purpose == ScanPurpose::create_snapshot)
	{
		populateCompletedScanDiagnostics(snapshot);
		saveCreatedSnapshot(snapshot);
		return;
	}

	m_currentSnapshot = completedSnapshot;
	populateDiagnostics();
	recalculateComparison(true);
}

void MainWindow::setScanActive(const bool active)
{
	m_ui->rootPathEdit->setEnabled(!active);
	m_ui->chooseRootButton->setEnabled(!active);
	m_ui->createSnapshotButton->setEnabled(!active);
	m_ui->compareSnapshotButton->setEnabled(!active);
	m_ui->cancelScanButton->setEnabled(active);
	m_ui->scanProgressBar->setVisible(active);
	m_ui->thresholdSpinBox->setEnabled(!active && m_baselineSnapshot && m_currentSnapshot);
}

void MainWindow::saveCreatedSnapshot(const Snapshot& snapshot)
{
	QStringList qualifications;
	if (!snapshot.diagnostics.empty())
	{
		const auto issueCount = static_cast<qulonglong>(snapshot.diagnostics.size());
		qualifications.push_back(QString{"%1 scan issue%2: %3"}
			.arg(issueCount).arg(issueCount == 1 ? "" : "s").arg(scanIssueSummary(snapshot.diagnostics)));
	}
	if (!snapshot.root.derived.subtreeCoverageComplete)
		qualifications.push_back("incomplete directory coverage");
	if (!snapshot.root.derived.subtreeAllocatedSize)
		qualifications.push_back("incomplete allocated-size accounting");
	if (!qualifications.empty())
	{
		const QString message = "The completed baseline is incomplete:\n\n- " + qualifications.join("\n- ")
			+ "\n\nSome contents or allocated sizes may be missing.\n\nSave this baseline anyway?";
		if (QMessageBox::warning(this, "Incomplete baseline", message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return;
	}

	CSettings settings;
	const QString lastUsedPath = settings.value(Settings::SavePath, QDir::currentPath()).toString();
	const QString defaultName = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm") + SnapshotExtension;
	QString destination = QFileDialog::getSaveFileName(
		this, "Save baseline", QDir{lastUsedPath}.filePath(defaultName), "SpaceGuard snapshots (*.spaceguard)");
	if (destination.isEmpty())
		return;
	if (!destination.endsWith(SnapshotExtension, Qt::CaseInsensitive))
		destination += SnapshotExtension;
	settings.setValue(Settings::SavePath, QFileInfo{destination}.absolutePath());

	const auto saved = snapshot.save(destination);
	if (!saved)
	{
		QMessageBox::critical(this, "Cannot save baseline", saveErrorDescription(saved.error()));
		return;
	}
	m_ui->scanStatusLabel->setText("Baseline saved to " + QDir::toNativeSeparators(destination));
}

void MainWindow::recalculateComparison(const bool reportError)
{
	if (!m_baselineSnapshot || !m_currentSnapshot)
		return;

	const uint64_t threshold = static_cast<uint64_t>(m_ui->thresholdSpinBox->value()) * BytesPerMiB;
	auto comparison = compareSnapshots(*m_baselineSnapshot, *m_currentSnapshot, threshold);
	if (!comparison)
	{
		clearComparisonDisplay();
		populateDiagnostics();
		const QString error = comparisonErrorDescription(comparison.error());
		m_ui->comparisonNoticeLabel->setText(error);
		if (reportError)
			QMessageBox::critical(this, "Snapshots cannot be compared", error);
		return;
	}
	m_comparison = std::move(*comparison);
	displayComparison();
}

void MainWindow::displayComparison()
{
	assert(m_baselineSnapshot);
	assert(m_currentSnapshot);
	assert(m_comparison);
	const SnapshotComparisonResult& comparison = *m_comparison;
	const auto& summary = comparison.summary;
	const uint64_t threshold = static_cast<uint64_t>(m_ui->thresholdSpinBox->value()) * BytesPerMiB;
	m_ui->comparisonContextLabel->setText(comparisonContext(*m_baselineSnapshot, *m_currentSnapshot));
	m_ui->comparisonHeadlineLabel->setText(comparisonHeadline(comparison, threshold));
	m_ui->wholeVolumeUsageValueLabel->setText(formatUsageChange(invertedChange(summary.freeSpaceChange)));
	m_ui->scannedTreeUsageValueLabel->setText(formatUsageChange(summary.allocatedTreeChange));
	m_ui->otherVolumeUsageValueLabel->setText(formatUsageChange(summary.unexplainedConsumptionChange));
	m_ui->comparisonDetailsLabel->setText(comparisonDetails(comparison));

	std::vector<ComparisonChange> changes = comparison.changes;
	std::sort(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
		return left.allocatedIncrease != right.allocatedIncrease
			? left.allocatedIncrease > right.allocatedIncrease : left.path < right.path;
	});
	m_ui->changesTable->setRowCount(static_cast<int>(changes.size()));
	for (int row = 0; row < static_cast<int>(changes.size()); ++row)
	{
		const auto& change = changes[static_cast<size_t>(row)];
		m_ui->changesTable->setItem(row, 0, createByteCountItem(change.allocatedIncrease));
		m_ui->changesTable->setItem(row, 1, new QTableWidgetItem{nativePathForDisplay(change.path)});
		auto* typeItem = new QTableWidgetItem{comparisonChangeType(change)};
		if (change.currentEntryKind == thin_io::entry_kind::directory)
			typeItem->setToolTip("Net allocated-size change for this folder's comparable subtree.");
		m_ui->changesTable->setItem(row, 2, typeItem);
		m_ui->changesTable->setItem(row, 3, createByteCountItem(change.baselineSubtreeAllocatedSize));
		m_ui->changesTable->setItem(row, 4, createByteCountItem(change.currentSubtreeAllocatedSize));
		setRowPath(*m_ui->changesTable, row, change.path);
	}

	m_ui->excludedTable->setRowCount(static_cast<int>(comparison.excludedRegions.size()));
	for (int row = 0; row < static_cast<int>(comparison.excludedRegions.size()); ++row)
	{
		const auto& region = comparison.excludedRegions[static_cast<size_t>(row)];
		m_ui->excludedTable->setItem(row, 0, new QTableWidgetItem{nativePathForDisplay(region.path)});
		m_ui->excludedTable->setItem(row, 1, new QTableWidgetItem{excludedRegionReason(region)});
		setRowPath(*m_ui->excludedTable, row, region.path);
	}

	populateDiagnostics();
	if (comparison.excludedRegions.empty())
		m_ui->comparisonNoticeLabel->clear();
	else
	{
		const auto excludedCount = static_cast<qulonglong>(comparison.excludedRegions.size());
		m_ui->comparisonNoticeLabel->setText(QString{"%1 location%2 could not be compared because scan coverage or allocated-size accounting was uncertain. Known growth elsewhere is still shown."}
			.arg(excludedCount).arg(excludedCount == 1 ? "" : "s"));
	}
}

void MainWindow::clearComparisonDisplay()
{
	m_comparison.reset();
	m_ui->comparisonContextLabel->setText("No comparison available.");
	m_ui->comparisonHeadlineLabel->clear();
	for (QLabel* label : {m_ui->wholeVolumeUsageValueLabel, m_ui->scannedTreeUsageValueLabel, m_ui->otherVolumeUsageValueLabel})
		label->setText("Unavailable");
	m_ui->comparisonNoticeLabel->clear();
	m_ui->comparisonDetailsLabel->clear();
	m_ui->changesTable->setRowCount(0);
	m_ui->excludedTable->setRowCount(0);
	m_ui->diagnosticsTable->setRowCount(0);
	m_ui->detailsTabs->setTabText(0, "Excluded regions");
	m_ui->detailsTabs->setTabText(1, "Scan issues");
	m_ui->detailsButton->setChecked(false);
	m_ui->detailsButton->setEnabled(false);
	m_ui->detailsButton->setText("Details");
	m_ui->thresholdSpinBox->setEnabled(false);
}

void MainWindow::populateDiagnostics()
{
	m_ui->diagnosticsTable->setRowCount(0);
	if (m_baselineSnapshot)
		appendSnapshotDiagnostics(*m_ui->diagnosticsTable, "Baseline", *m_baselineSnapshot);
	if (m_currentSnapshot)
		appendSnapshotDiagnostics(*m_ui->diagnosticsTable, "Current scan", *m_currentSnapshot);
	updateDetailsDisclosure();
}

void MainWindow::populateCompletedScanDiagnostics(const Snapshot& snapshot)
{
	m_ui->diagnosticsTable->setRowCount(0);
	appendSnapshotDiagnostics(*m_ui->diagnosticsTable, "Current scan", snapshot);
	updateDetailsDisclosure();
	if (!snapshot.diagnostics.empty())
	{
		m_ui->detailsTabs->setCurrentIndex(1);
		m_ui->detailsButton->setChecked(true);
	}
}

void MainWindow::updateDetailsDisclosure()
{
	const size_t excludedCount = m_comparison ? m_comparison->excludedRegions.size() : 0;
	const int diagnosticCount = m_ui->diagnosticsTable->rowCount();
	m_ui->detailsTabs->setTabText(0, QString{"Excluded regions (%1)"}.arg(static_cast<qulonglong>(excludedCount)));
	m_ui->detailsTabs->setTabText(1, QString{"Scan issues (%1)"}.arg(diagnosticCount));

	QStringList counts;
	if (excludedCount != 0)
		counts.push_back(QString{"%1 excluded region%2"}.arg(static_cast<qulonglong>(excludedCount)).arg(excludedCount == 1 ? "" : "s"));
	if (diagnosticCount != 0)
		counts.push_back(QString{"%1 scan issue%2"}.arg(diagnosticCount).arg(diagnosticCount == 1 ? "" : "s"));
	m_ui->detailsButton->setText(counts.empty() ? "Details" : "Details (" + counts.join(", ") + ")");

	const bool detailsAvailable = m_comparison.has_value() || diagnosticCount != 0;
	m_ui->detailsButton->setEnabled(detailsAvailable);
	if (!detailsAvailable)
		m_ui->detailsButton->setChecked(false);
}

void MainWindow::openTableItem(const QTableWidgetItem* item)
{
	if (!item || !item->data(Qt::UserRole).isValid())
		return;
	const NativePath path = storedNativePath(*item);
	bool started = false;
#ifdef _WIN32
	started = QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(path)});
#elif defined(__APPLE__)
	started = QProcess::startDetached("/usr/bin/open", {"-R", QString::fromLatin1(nativePathFileUrl(path))});
#elif defined(__linux__)
	const QString url = QString::fromLatin1(nativePathFileUrl(path));
	started = QProcess::startDetached("dbus-send", {"--session", "--dest=org.freedesktop.FileManager1", "--type=method_call",
		"/org/freedesktop/FileManager1", "org.freedesktop.FileManager1.ShowItems", "array:string:" + url, "string:"});
#else
	started = QDesktopServices::openUrl(QUrl::fromEncoded(nativePathFileUrl(path)));
#endif
	if (!started)
		m_ui->scanStatusLabel->setText("Could not open " + nativePathForDisplay(path));
}
