TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = cpputils thin_io spaceguard_testapp

cpputils.file = $${PWD}/../cpputils/cpputils.pro
thin_io.file = $${PWD}/../thin_io/thin_io.pro
spaceguard_testapp.subdir = $${PWD}/spaceguard_testapp

spaceguard_testapp.depends = cpputils thin_io
