TEMPLATE = lib
CONFIG += staticlib
TARGET = msyncd
QT += xml \
    dbus \
    sql \
    network
QT -= gui

CONFIG += \
    link_pkgconfig \
    create_prl

DEPENDPATH += .
INCLUDEPATH += . \
    ../ \
    ../libbuteosyncfw/pluginmgr \
    ../libbuteosyncfw/common \
    ../libbuteosyncfw/profile


PKGCONFIG += dbus-1 gio-2.0 libsignon-qt$${QT_MAJOR_VERSION} accounts-qt$${QT_MAJOR_VERSION}
packagesExist(mce-qt$${QT_MAJOR_VERSION}) {
    PKGCONFIG += mce-qt$${QT_MAJOR_VERSION}
    DEFINES += HAS_MCE
} else {
    message("mce-qt not found, MCE support disabled")
}
LIBS += -lbuteosyncfw$${QT_MAJOR_VERSION}
packagesExist(qt$${QT_MAJOR_VERSION}-boostable) {
    DEFINES += HAS_BOOSTER
    PKGCONFIG += qt$${QT_MAJOR_VERSION}-boostable
} else {
    warning("qt$${QT_MAJOR_VERSION}-boostable not available; startup times will be slower")
}

QMAKE_LIBDIR_QT += ../libsyncprofile/

LIBS += -L../libbuteosyncfw

# Input
HEADERS += ServerActivator.h \
    synchronizer.h \
    SyncDBusInterface.h \
    SyncBackupProxy.h \
    SyncDBusAdaptor.h \
    SyncBackupAdaptor.h \
    ClientThread.h \
    ServerThread.h \
    StorageBooker.h \
    SyncQueue.h \
    SyncScheduler.h \
    SyncBackup.h \
    AccountsHelper.h \
    SyncSession.h \
    PluginRunner.h \
    ClientPluginRunner.h \
    ServerPluginRunner.h \
    SyncSigHandler.h \
    StorageChangeNotifier.h \
    SyncOnChange.h \
    SyncOnChangeScheduler.h

SOURCES += ServerActivator.cpp \
    synchronizer.cpp \
    SyncDBusAdaptor.cpp \
    SyncBackupAdaptor.cpp \
    ClientThread.cpp \
    ServerThread.cpp \
    StorageBooker.cpp \
    SyncQueue.cpp \
    SyncScheduler.cpp \
    SyncBackup.cpp \
    AccountsHelper.cpp \
    SyncSession.cpp \
    PluginRunner.cpp \
    ClientPluginRunner.cpp \
    ServerPluginRunner.cpp \
    SyncSigHandler.cpp \
    StorageChangeNotifier.cpp \
    SyncOnChange.cpp \
    SyncOnChangeScheduler.cpp

contains(DEFINES, USE_KEEPALIVE) {
    message("Use keepalive")
    PKGCONFIG += keepalive

    HEADERS += \
        BackgroundSync.h

    SOURCES += \
        BackgroundSync.cpp

} else {
    message("Use libiphb")
    PKGCONFIG += libiphb

    HEADERS += \
        IPHeartBeat.h \
        SyncAlarmInventory.h

    SOURCES += \
        IPHeartBeat.cpp \
        SyncAlarmInventory.cpp
}
