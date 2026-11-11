QT += core gui concurrent widgets printsupport qml quick quickcontrols2 quickdialogs2 dbus

CONFIG += c++17 release
TARGET = omawrite
TEMPLATE = app

HEADERS += \
    src/backend.h \
    src/blockrenderer.h \
    src/cli.h \
    src/markdownhighlighter.h \
    src/systemtheme.h

SOURCES += \
    src/main.cpp \
    src/backend.cpp \
    src/blockrenderer.cpp \
    src/cli.cpp \
    src/markdownhighlighter.cpp \
    src/systemtheme.cpp

RESOURCES += src/resources.qrc

# Where `omawrite --update` rebuilds from: the makepkg build runs in this
# checkout, so the installed binary always knows its own source.
DEFINES += OMAWRITE_SOURCE_DIR=\\\"$$PWD\\\"
