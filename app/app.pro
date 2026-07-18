###################################################
#            Basic configuration
###################################################

TEMPLATE = app
TARGET   = SpaceGuard

QT = core gui widgets

CONFIG += strict_c++ c++2b

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

Release:OUTPUT_DIR=release/
Debug:OUTPUT_DIR=debug/

DESTDIR  = ../bin/$${OUTPUT_DIR}
OBJECTS_DIR = ../build/$${OUTPUT_DIR}/$${TARGET}
MOC_DIR     = ../build/$${OUTPUT_DIR}/$${TARGET}
UI_DIR      = ../build/$${OUTPUT_DIR}/$${TARGET}
RCC_DIR     = ../build/$${OUTPUT_DIR}/$${TARGET}

###################################################
#               INCLUDEPATH
###################################################

INCLUDEPATH += \
	../qtutils \
	../cpputils \
	../cpp-template-utils \
	../thin_io/src

###################################################
#                 SOURCES
###################################################

SOURCES += \
	src/main.cpp \
	src/mainwindow.cpp \
	src/native_path.cpp \
	src/snapshot.cpp \
	src/snapshot_comparison.cpp \
	src/snapshot_scan_runner.cpp \
	src/snapshot_scanner.cpp

###################################################
#                 LIBS
###################################################

LIBS += -L$${DESTDIR} -lqtutils -lcpputils -lthin_io

mac*|linux*|freebsd*{
	PRE_TARGETDEPS += \
		$${DESTDIR}/libcpputils.a \
		$${DESTDIR}/libthin_io.a
}

###################################################
#    Platform-specific compiler options and libs
###################################################

win*{
	#LIBS += -lole32 -lShell32 -lUser32
	QMAKE_CXXFLAGS += /MP /wd4251
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS

	QMAKE_LFLAGS += /DEBUG:FASTLINK /TIME

	Debug:QMAKE_LFLAGS += /INCREMENTAL
	Release:QMAKE_LFLAGS += /OPT:REF /OPT:ICF
}

mac*{
	LIBS += -framework AppKit

	QMAKE_POST_LINK = cp -f -p $${DESTDIR}/*.dylib $${DESTDIR}/$${TARGET}.app/Contents/MacOS/ || true
}

###################################################
#      Generic stuff for Linux and Mac
###################################################

linux*|mac*|freebsd {
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}

FORMS += \
	src/mainwindow.ui

HEADERS += \
	src/filesystem_access.h \
	src/mainwindow.h \
	src/native_path.h \
	src/settings.h \
	src/snapshot.h \
	src/snapshot_comparison.h \
	src/snapshot_internal.h \
	src/snapshot_scan_runner.h \
	src/snapshot_scanner.h
