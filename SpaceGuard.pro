TEMPLATE = subdirs

SUBDIRS += app qtutils cpputils cpp-template-utils

qtutils.depends = cpputils cpp-template-utils
app.depends = qtutils cpputils cpp-template-utils
