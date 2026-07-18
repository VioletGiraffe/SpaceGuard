TEMPLATE = app
TARGET = spaceguard_testapp

QT = core

CONFIG += console strict_c++ c++2b
CONFIG -= app_bundle

mac* | linux* | freebsd {
	CONFIG(release, debug|release):CONFIG *= Release optimize_full
	CONFIG(debug, debug|release):CONFIG *= Debug
}

Release:OUTPUT_DIR=release
Debug:OUTPUT_DIR=debug

DESTDIR = ../../bin/$${OUTPUT_DIR}
OBJECTS_DIR = ../../build/$${OUTPUT_DIR}/$${TARGET}
MOC_DIR = ../../build/$${OUTPUT_DIR}/$${TARGET}
UI_DIR = ../../build/$${OUTPUT_DIR}/$${TARGET}
RCC_DIR = ../../build/$${OUTPUT_DIR}/$${TARGET}

INCLUDEPATH += \
	../../app/src \
	../../cpp-template-utils \
	../../thin_io/src

LIBS += -L$${DESTDIR} -lthin_io

mac*|linux*|freebsd*{
	PRE_TARGETDEPS += $${DESTDIR}/libthin_io.a
}

win*{
	QMAKE_CXXFLAGS += /MP /wd4251
	QMAKE_CXXFLAGS += /std:c++latest /permissive- /Zc:__cplusplus /FS
	QMAKE_CXXFLAGS_WARN_ON = /W4
	DEFINES += WIN32_LEAN_AND_MEAN NOMINMAX _SCL_SECURE_NO_WARNINGS

	QMAKE_LFLAGS += /DEBUG:FASTLINK

	Debug:QMAKE_LFLAGS += /INCREMENTAL
	Release:QMAKE_LFLAGS += /OPT:REF /OPT:ICF
}

linux*|mac*|freebsd {
	QMAKE_CXXFLAGS_WARN_ON = -Wall -Wextra

	Release:DEFINES += NDEBUG=1
	Debug:DEFINES += _DEBUG
}

SOURCES += \
	../../app/src/filesystem_access.cpp \
	../../app/src/native_path.cpp \
	../../app/src/snapshot.cpp \
	test_filesystem_access.cpp \
	test_native_path.cpp \
	test_snapshot.cpp \
	tests_main.cpp

HEADERS += \
	../../app/src/filesystem_access.h \
	../../app/src/native_path.h \
	../../app/src/snapshot.h
