TEMPLATE = subdirs

SUBDIRS += app qtutils cpputils cpp-template-utils thin_io

qtutils.depends = cpputils cpp-template-utils
app.depends = qtutils cpputils cpp-template-utils thin_io
