TEMPLATE = lib
TARGET = buteoprofiles
PLUGIN_IMPORT_PATH = Buteo/Profiles
QT -= gui
QT += qml dbus
CONFIG += qt plugin hide_symbols

INCLUDEPATH += ../libbuteosyncfw ../libbuteosyncfw/common ../libbuteosyncfw/profile

LIBS += -L../libbuteosyncfw -lbuteosyncfw$${QT_MAJOR_VERSION}
target.path = $$[QT_INSTALL_QML]/$$PLUGIN_IMPORT_PATH

SOURCES += plugin.cpp \
    syncresultmodelbase.cpp \
    syncresultmodel.cpp \
    multisyncresultmodel.cpp \
    syncprofilewatcher.cpp \
    syncmanager.cpp

HEADERS += syncresultmodelbase.h \
    syncresultmodel.h \
    multisyncresultmodel.h \
    syncprofilewatcher.h \
    profileentry.h \
    syncmanager.h

OTHER_FILES += qmldir

qmldir.files += qmldir
qmldir.path += $$target.path

INSTALLS += target qmldir
