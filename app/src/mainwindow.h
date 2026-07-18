#pragma once

#include "snapshot_comparison.h"
#include "snapshot_scan_runner.h"

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
		create_snapshot,
		compare_with_snapshot
	};

	void chooseRootDirectory();
	void createSnapshot();
	void compareWithSnapshot();
	void cancelScan();
	void beginScan(ScanPurpose purpose, const SpaceGuard::NativePath& rootPath);
	void updateScanProgress(uint64_t generation, const SpaceGuard::SnapshotScanProgress& progress);
	void scanCompleted(uint64_t generation, const std::shared_ptr<const SpaceGuard::SnapshotScanResult>& result);
	void setScanActive(bool active);

	void saveCreatedSnapshot(const SpaceGuard::Snapshot& snapshot);
	void recalculateComparison(bool reportError = false);
	void displayComparison(const SpaceGuard::SnapshotComparisonResult& comparison);
	void clearComparisonDisplay();
	void populateDiagnostics();
	void openTableItem(const QTableWidgetItem* item);

	std::unique_ptr<Ui::MainWindow> m_ui;
	SpaceGuard::ThinIoFilesystemAccess m_filesystem;
	CExecutionQueue m_publicationQueue;
	SpaceGuard::SnapshotScanRunner m_scanRunner;
	QTimer m_publicationTimer;

	std::optional<uint64_t> m_activeGeneration;
	std::optional<ScanPurpose> m_activePurpose;
	std::shared_ptr<const SpaceGuard::Snapshot> m_baselineSnapshot;
	std::shared_ptr<const SpaceGuard::Snapshot> m_currentSnapshot;
};
