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
#include <QMessageBox>
#include <QProcess>
#include <QTableWidget>
#include <QUrl>

#include <algorithm>
#include <assert.h>
#include <exception>
#include <utility>

namespace {

constexpr auto SnapshotExtension = ".spaceguard";
constexpr uint64_t BytesPerMiB = 1024 * 1024;

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
	m_ui->changesTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
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
		m_ui->scanStatusLabel->setText(purpose == ScanPurpose::create_snapshot ? "Creating snapshot..." : "Scanning current state...");
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
		QMessageBox::critical(this, "Cannot start scan", "The background scan thread could not be started.");
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
	m_ui->scanStatusLabel->setText(QString{"Scan complete: %1 issue(s)."}.arg(static_cast<qulonglong>(snapshot.diagnostics.size())));
	m_ui->scanDurationLabel->setText("Scan time: " + elapsedTime);
	if (purpose == ScanPurpose::create_snapshot)
	{
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
		qualifications.push_back(QString{"%1 scan issue(s)"}.arg(static_cast<qulonglong>(snapshot.diagnostics.size())));
	if (!snapshot.root.derived.subtreeCoverageComplete)
		qualifications.push_back("incomplete directory coverage");
	if (!snapshot.root.derived.subtreeAllocatedSize)
		qualifications.push_back("incomplete allocated-size accounting");
	if (!qualifications.empty())
	{
		const QString message = "The completed snapshot has " + qualifications.join(", ")
			+ ". Some contents or allocated sizes may be missing.\n\nSave this snapshot anyway?";
		if (QMessageBox::warning(this, "Incomplete snapshot", message, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return;
	}

	CSettings settings;
	const QString lastUsedPath = settings.value(Settings::SavePath, QDir::currentPath()).toString();
	const QString defaultName = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm") + SnapshotExtension;
	QString destination = QFileDialog::getSaveFileName(
		this, "Save snapshot", QDir{lastUsedPath}.filePath(defaultName), "SpaceGuard snapshots (*.spaceguard)");
	if (destination.isEmpty())
		return;
	if (!destination.endsWith(SnapshotExtension, Qt::CaseInsensitive))
		destination += SnapshotExtension;
	settings.setValue(Settings::SavePath, QFileInfo{destination}.absolutePath());

	const auto saved = snapshot.save(destination);
	if (!saved)
	{
		QMessageBox::critical(this, "Cannot save snapshot", saveErrorDescription(saved.error()));
		return;
	}
	m_ui->scanStatusLabel->setText("Snapshot saved to " + QDir::toNativeSeparators(destination));
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
	displayComparison(*comparison);
}

void MainWindow::displayComparison(const SnapshotComparisonResult& comparison)
{
	const auto& summary = comparison.summary;
	m_ui->freeSpaceValueLabel->setText(formatChange(summary.freeSpaceChange));
	m_ui->availableSpaceValueLabel->setText(formatChange(summary.availableSpaceChange));
	m_ui->allocatedTreeValueLabel->setText(formatChange(summary.allocatedTreeChange));
	m_ui->unexplainedValueLabel->setText(formatChange(summary.unexplainedConsumptionChange));
	m_ui->capacityValueLabel->setText(formatChange(summary.capacityChange));
	switch (summary.reconciliation)
	{
	case ReconciliationState::exact: m_ui->reconciliationValueLabel->setText("Exact"); break;
	case ReconciliationState::incomplete: m_ui->reconciliationValueLabel->setText("Incomplete"); break;
	case ReconciliationState::overflow: m_ui->reconciliationValueLabel->setText("Overflow"); break;
	}

	std::vector<ComparisonChange> changes = comparison.changes;
	std::sort(changes.begin(), changes.end(), [](const auto& left, const auto& right) {
		return left.allocatedIncrease != right.allocatedIncrease
			? left.allocatedIncrease > right.allocatedIncrease : left.path < right.path;
	});
	m_ui->changesTable->setRowCount(static_cast<int>(changes.size()));
	for (int row = 0; row < static_cast<int>(changes.size()); ++row)
	{
		const auto& change = changes[static_cast<size_t>(row)];
		m_ui->changesTable->setItem(row, 0, new QTableWidgetItem{formatBytes(change.allocatedIncrease)});
		m_ui->changesTable->setItem(row, 1, new QTableWidgetItem{nativePathForDisplay(change.path)});
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
	m_ui->resultTabs->setTabText(0, QString{"Changes (%1)"}.arg(static_cast<qulonglong>(changes.size())));
	m_ui->resultTabs->setTabText(1, QString{"Excluded regions (%1)"}.arg(static_cast<qulonglong>(comparison.excludedRegions.size())));
	m_ui->resultTabs->setTabText(2, QString{"Scan issues (%1)"}.arg(m_ui->diagnosticsTable->rowCount()));

	QStringList notices;
	for (const SnapshotComparisonWarning warning : comparison.warnings)
	{
		if (warning == SnapshotComparisonWarning::root_identity_unavailable)
			notices.push_back("The root identity was unavailable, so root replacement could not be verified.");
		else
			notices.push_back("The filesystem identity was unavailable, so filesystem replacement could not be verified.");
	}
	if (summary.baselineScanFreeSpaceChange && summary.baselineScanFreeSpaceChange->direction != ChangeDirection::unchanged)
		notices.push_back("Free space changed by " + formatChange(summary.baselineScanFreeSpaceChange) + " while the baseline scan was running.");
	if (summary.currentScanFreeSpaceChange && summary.currentScanFreeSpaceChange->direction != ChangeDirection::unchanged)
		notices.push_back("Free space changed by " + formatChange(summary.currentScanFreeSpaceChange) + " while the current scan was running.");
	if (!comparison.excludedRegions.empty())
		notices.push_back(QString{"%1 region(s) were excluded because their scan coverage or accounting was uncertain."}
			.arg(static_cast<qulonglong>(comparison.excludedRegions.size())));
	const int diagnosticCount = m_ui->diagnosticsTable->rowCount();
	if (diagnosticCount != 0)
		notices.push_back(QString{"%1 scan issue(s) are listed in the details tab."}.arg(diagnosticCount));
	m_ui->comparisonNoticeLabel->setText(notices.join(" "));
}

void MainWindow::clearComparisonDisplay()
{
	for (QLabel* label : {m_ui->freeSpaceValueLabel, m_ui->availableSpaceValueLabel, m_ui->allocatedTreeValueLabel,
		m_ui->unexplainedValueLabel, m_ui->capacityValueLabel})
		label->setText("Unavailable");
	m_ui->reconciliationValueLabel->setText("Incomplete");
	m_ui->comparisonNoticeLabel->clear();
	m_ui->changesTable->setRowCount(0);
	m_ui->excludedTable->setRowCount(0);
	m_ui->diagnosticsTable->setRowCount(0);
	m_ui->resultTabs->setTabText(0, "Changes");
	m_ui->resultTabs->setTabText(1, "Excluded regions");
	m_ui->resultTabs->setTabText(2, "Scan issues");
	m_ui->thresholdSpinBox->setEnabled(false);
}

void MainWindow::populateDiagnostics()
{
	m_ui->diagnosticsTable->setRowCount(0);
	const auto addSnapshotDiagnostics = [this](const QString& source, const Snapshot& snapshot) {
		for (const SnapshotDiagnostic& diagnostic : snapshot.diagnostics)
		{
			const int row = m_ui->diagnosticsTable->rowCount();
			m_ui->diagnosticsTable->insertRow(row);
			m_ui->diagnosticsTable->setItem(row, 0, new QTableWidgetItem{source});
			m_ui->diagnosticsTable->setItem(row, 1, new QTableWidgetItem{diagnosticOperationName(diagnostic.operation)});
			m_ui->diagnosticsTable->setItem(row, 2, new QTableWidgetItem{nativeErrorCodeText(diagnostic.nativeErrorCode)});
			m_ui->diagnosticsTable->setItem(row, 3, new QTableWidgetItem{nativeErrorDescription(diagnostic.nativeErrorCode)});
			m_ui->diagnosticsTable->setItem(row, 4, new QTableWidgetItem{nativePathForDisplay(diagnostic.path)});
			setRowPath(*m_ui->diagnosticsTable, row, diagnostic.path);
		}
	};

	if (m_baselineSnapshot)
		addSnapshotDiagnostics("Baseline", *m_baselineSnapshot);
	if (m_currentSnapshot)
		addSnapshotDiagnostics("Current", *m_currentSnapshot);
	m_ui->resultTabs->setTabText(2, QString{"Scan issues (%1)"}.arg(m_ui->diagnosticsTable->rowCount()));
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
