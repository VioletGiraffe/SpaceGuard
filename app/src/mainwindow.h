#pragma once
#include "snapshot.h"

#include <QMainWindow>

#include <optional>

namespace Ui {
class MainWindow;
}

class MainWindow final : public QMainWindow
{
public:
	explicit MainWindow(QWidget *parent = nullptr);
	~MainWindow();

private:
	void onSave();
	void onLoad();
	void onCompare();

	std::optional<Snapshot> takeSnapshot();

	static void openInExplorer(const QString& path);

	void calculateDiffAndDisplayResult();

private:
	std::optional<Snapshot> _loadedSnapshot;
	std::optional<Snapshot> _currentSnapshot;
	Ui::MainWindow *ui;
};
