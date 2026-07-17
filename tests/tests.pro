TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS = thin_io spaceguard_testapp

thin_io.file = $${PWD}/../thin_io/thin_io.pro
spaceguard_testapp.subdir = $${PWD}/spaceguard_testapp

spaceguard_testapp.depends = thin_io
