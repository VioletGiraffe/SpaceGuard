TEMPLATE = subdirs

SUBDIRS += app qtutils cpputils cpp-template-utils

app.depends = qtutils cpputils cpp-template-utils
