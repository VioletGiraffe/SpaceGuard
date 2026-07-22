#pragma once

#include "snapshot_comparison.h"
#include "snapshot_scan_runner.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QTimer>

#include <memory>
#include <optional>
#include <stdint.h>

class QTableWidgetItem;

namespace Ui {
class MainWindow;
}

class MainWindow final : public QMainWindow
{
public:
	explicit MainWindow(QWidget* parent = nullptr);
	~MainWindow();

private:
	enum class ScanPurpose : uint8_t {
		create_baseline,
		compare_with_baseline,
		inspect_current_usage
	};

	void chooseRootDirectory();
	void createBaseline();
	void findGrowth();
	void inspectCurrentUsage();
	void cancelScan();
	[[nodiscard]] std::optional<NativePath> validatedSelectedRootPath();
	void beginScan(ScanPurpose purpose, const NativePath& rootPath);
	void updateScanProgress(uint64_t generation, const SnapshotScanProgress& progress);
	void scanCompleted(uint64_t generation, const std::shared_ptr<const SnapshotScanResult>& result);
	void setScanActive(bool active);
	void clearCurrentSnapshot();
	void adoptCurrentSnapshot(std::shared_ptr<const Snapshot> snapshot);

	void saveCreatedSnapshot(const Snapshot& snapshot);
	void recalculateComparison(bool reportError = false);
	void displayComparison();
	void clearComparisonDisplay();
	void populateDiagnostics();
	void populateCompletedScanDiagnostics(const Snapshot& snapshot);
	void updateDetailsDisclosure();
	void updateGrowthActions();
	void showSelectedGrowthInCurrentUsage();
	void revealSelectedGrowthInFileManager();
	void revealTableItemInFileManager(const QTableWidgetItem* item);
	void revealPath(const NativePath& path);

	std::unique_ptr<Ui::MainWindow> m_ui;
	CExecutionQueue m_publicationQueue;
	SnapshotScanRunner m_scanRunner;
	QTimer m_publicationTimer;
	QElapsedTimer m_scanElapsedTimer;
	QTimer m_scanElapsedUpdateTimer;

	std::optional<uint64_t> m_activeGeneration;
	std::optional<ScanPurpose> m_activePurpose;
	std::shared_ptr<const Snapshot> m_baselineSnapshot;
	std::shared_ptr<const Snapshot> m_currentSnapshot;
	std::optional<SnapshotComparisonResult> m_comparison;
};
