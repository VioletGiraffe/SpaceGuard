#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "settings.h"

#include "snapshot.h"

#include "settings/csettings.h"

#include <qdatetime.h>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QProcess>

#define SnapshotExtension "spaceguard"

inline QString toVolume(qint64 bytes)
{
	if (bytes < 1024)
		return QString::number(bytes) + " B";
	if (bytes < 1024 * 1024)
		return QString::number((float)bytes / 1024.0f, 'f', 1) + " KiB";
	if (bytes < 1024 * 1024 * 1024)
		return QString::number((float)bytes / (1024.0f * 1024.0f), 'f', 1) + " MiB";
	else
		return QString::number((float)bytes / (1024.0f * 1024.0f * 1024.0f), 'f', 1) + " GiB";
}

MainWindow::MainWindow(QWidget *parent) :
	QMainWindow(parent),
	ui(new Ui::MainWindow)
{
	ui->setupUi(this);

	connect(ui->btnSave, &QPushButton::clicked, this, &MainWindow::onSave);
	connect(ui->btnLoad, &QPushButton::clicked, this, &MainWindow::onLoad);
	connect(ui->btnCompare, &QPushButton::clicked, this, &MainWindow::onCompare);

	connect(ui->threshold, &QSpinBox::valueChanged, this, &MainWindow::calculateDiffAndDisplayResult);

	connect(ui->btnChooseDirectory, &QAbstractButton::clicked, this, [this]{
		const auto path = QFileDialog::getExistingDirectory(this);
		ui->pathToAnalyze->setText(path);
	});

	connect(ui->table, &QTableWidget::itemActivated, this, [](QTableWidgetItem* item) {
		openInExplorer(item->data(Qt::UserRole).toString());
	});

	CSettings s;
	ui->pathToAnalyze->setText(s.value(Settings::Path).toString());
	ui->threshold->setValue(s.value(Settings::Threshold, 1024).toInt());
}

MainWindow::~MainWindow()
{
	CSettings s;
	s.setValue(Settings::Path, ui->pathToAnalyze->text());
	s.setValue(Settings::Threshold, ui->threshold->value());

	delete ui;
}

void MainWindow::onSave()
{
	CSettings s;

	const auto lastUsedPath = s.value(Settings::SavePath, QDir::currentPath()).toString();
	const QString defaultName = QDateTime::currentDateTime().toString("dd-MM-yy hh-mm.") + SnapshotExtension;

	const auto saveTo = QFileDialog::getSaveFileName(this, {}, lastUsedPath + "/" + defaultName, "*." SnapshotExtension);
	if (saveTo.isEmpty())
		return;

	s.setValue(Settings::SavePath, QFileInfo{ saveTo }.absolutePath());

	const auto snap = takeSnapshot();
	if (snap && !snap->save(saveTo))
		QMessageBox::critical(this, {}, "Failed to save the snapshot to\n" + saveTo);
}

void MainWindow::onLoad()
{
	const auto lastUsedPath = CSettings{}.value(Settings::SavePath, QDir::currentPath()).toString();

	const auto path = QFileDialog::getOpenFileName(this, {}, lastUsedPath, "*." SnapshotExtension);
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

void MainWindow::onCompare()
{
	if (!_loadedSnapshot)
	{
		const auto result = QMessageBox::information(this, {}, "No snapshot loaded to compare to, do you want to load a snapshot now?", QMessageBox::StandardButton::Yes | QMessageBox::StandardButton::No);
		if (result == QMessageBox::StandardButton::Yes)
			onLoad();

		if (!_loadedSnapshot)
			return;
	}

	_currentSnapshot = takeSnapshot();
	calculateDiffAndDisplayResult();
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

void MainWindow::openInExplorer(const QString& path)
{
#ifdef _WIN32
	QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(path)});
#elif defined(__APPLE__)    //Code for Mac
	QProcess::execute("/usr/bin/osascript", {"-e", "tell application \"Finder\" to reveal POSIX file \"" + path + "\""});
	QProcess::execute("/usr/bin/osascript", {"-e", "tell application \"Finder\" to activate"});
#elif defined(Q_OS_LINUX)
	QStringList args;
	args << "--session";
	args << "--dest=org.freedesktop.FileManager1";
	args << "--type=method_call";
	args << "/org/freedesktop/FileManager1";
	args << "org.freedesktop.FileManager1.ShowItems";
	args << "array:string:file://" + path;
	args << "string:";
	QProcess::startDetached("dbus-send", args);
#endif
}

void MainWindow::calculateDiffAndDisplayResult()
{
	if (!_currentSnapshot || !_loadedSnapshot)
		return;

	if (_currentSnapshot->path() != _loadedSnapshot->path())
	{
		QMessageBox::critical(this, {}, "Loaded snapshot path doesn't matched the current path:\n" + _loadedSnapshot->path() + "\n" + _currentSnapshot->path());
		return;
	}

	const qint64 thresholdBytes = (qint64)ui->threshold->value() * 1024LL * 1024LL;
	QList<Snapshot::Change> diff = Snapshot::compare(*_loadedSnapshot, *_currentSnapshot, thresholdBytes);
	std::sort(diff.begin(), diff.end(), [](const auto& a, const auto& b) { return a.sizeIncrease > b.sizeIncrease; });

	ui->table->setUpdatesEnabled(false);
	ui->table->clearContents();
	ui->table->setRowCount(diff.size());

	for (qsizetype i = 0; i < diff.size(); ++i)
	{
		const auto& change = diff[i];
		QString displayedPath = change.path;
		if (displayedPath.endsWith('/'))
		{
			displayedPath.chop(1);
			displayedPath = '[' + displayedPath + ']';
		}

		auto* sizeItem = new QTableWidgetItem(toVolume(change.sizeIncrease));
		auto* pathItem = new QTableWidgetItem(displayedPath);

		sizeItem->setData(Qt::UserRole, change.path);
		pathItem->setData(Qt::UserRole, change.path);

		ui->table->setItem(i, 0, sizeItem);
		ui->table->setItem(i, 1, pathItem);
	}

	ui->table->setUpdatesEnabled(true);
	ui->table->resizeColumnsToContents();
}
