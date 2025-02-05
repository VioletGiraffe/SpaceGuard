#include "mainwindow.h"
#include "ui_mainwindow.h"

#include "snapshot.h"

#include <qdatetime.h>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>

#define SnapshotExtension "spaceguard"

MainWindow::MainWindow(QWidget *parent) :
	QMainWindow(parent),
	ui(new Ui::MainWindow)
{
	ui->setupUi(this);

	connect(ui->btnSave, &QPushButton::clicked, this, &MainWindow::save);
	connect(ui->btnLoad, &QPushButton::clicked, this, &MainWindow::load);
	connect(ui->btnCompare, &QPushButton::clicked, this, &MainWindow::compare);
	connect(ui->btnChooseDirectory, &QAbstractButton::clicked, this, [this]{
		const auto path = QFileDialog::getExistingDirectory(this);
		ui->pathToAnalyze->setText(path);
	});
}

MainWindow::~MainWindow()
{
	delete ui;
}

void MainWindow::save()
{
	const QString defaultName = QDateTime::currentDateTime().toString("dd-MM-yy hh-mm.") + SnapshotExtension;
	const auto saveTo = QFileDialog::getSaveFileName(this, {}, QDir::currentPath() + "/" + defaultName, "*." SnapshotExtension);
	if (saveTo.isEmpty())
		return;

	const auto snap = takeSnapshot();
	if (snap && !snap->save(saveTo))
		QMessageBox::critical(this, {}, "Failed to save the snapshot to\n" + saveTo);
}

void MainWindow::load()
{
	const auto path = QFileDialog::getOpenFileName(this, {}, "*." SnapshotExtension);
	if (path.isEmpty())
		return;

	_loadedSnapshot = Snapshot{};
	if (!_loadedSnapshot->load(path))
	{
		_loadedSnapshot.reset();
		QMessageBox::critical(this, {}, "Failed to load the snapshot from\n" + path);
		return;
	}

	ui->pathToAnalyze->setText(_loadedSnapshot->path());
}

void MainWindow::compare()
{
	if (!_loadedSnapshot)
	{
		const auto result = QMessageBox::information(this, {}, "No snapshot loaded to compare to, do you want to load a snapshot now?", QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No);
		if (result == QMessageBox::StandardButton::Yes)
			load();

		if (!_loadedSnapshot)
			return;
	}

	const auto snap = takeSnapshot();
	if (!snap)
		return;

	const QStringList diff = Snapshot::compare(*_loadedSnapshot, *snap, 512 * 1024);
	ui->text->setPlainText(diff.join("\n\n"));
}

std::optional<Snapshot> MainWindow::takeSnapshot()
{
	QString path = ui->pathToAnalyze->text();
	if (!path.isEmpty() && !path.endsWith('/'))
		path += '/';

	if (path.isEmpty() || !QDir{path}.exists())
	{
		QMessageBox::warning(this, {}, "The specified path is invalid or doesn't exist.");
		return {};
	}

	return Snapshot::create(path);
}
