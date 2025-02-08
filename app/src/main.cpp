 #include "mainwindow.h"

 #include <QApplication>

 int main(int argc, char* argv[])
 {
	QApplication app{argc, argv};

	app.setOrganizationName("GitHubSoft");
	app.setApplicationName("SpaceGuard");

	MainWindow w;
	w.show();

	return app.exec();
 }
