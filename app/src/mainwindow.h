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
	void save();
	void load();
	void compare();

	std::optional<Snapshot> takeSnapshot();

private:
	std::optional<Snapshot> _loadedSnapshot;
	Ui::MainWindow *ui;
};
