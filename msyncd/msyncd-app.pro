TEMPLATE = app
TARGET = msyncd
QT += xml \
    dbus \
    sql \
    network
QT -= gui

CONFIG += \
    link_pkgconfig \
    link_prl

DEPENDPATH += .
INCLUDEPATH += . \
    ../ \
    ../libbuteosyncfw/pluginmgr \
    ../libbuteosyncfw/common \
    ../libbuteosyncfw/profile
PRE_TARGETDEPS += libmsyncd.a


PKGCONFIG += dbus-1 libsignon-qt5 accounts-qt5
LIBS += -lbuteosyncfw5
packagesExist(qt5-boostable) {
    DEFINES += HAS_BOOSTER
    PKGCONFIG += qt5-boostable
} else {
    warning("qt5-boostable not available; startup times will be slower")
}

QMAKE_LIBDIR_QT += ../libsyncprofile/

LIBS += -L../libbuteosyncfw

LIBS += -L. -lmsyncd

# Input
SOURCES += \
        main.cpp \
        UnitTest.cpp

# install
target.path = /usr/bin/
service.files = bin/msyncd.service
service.path = /usr/lib/systemd/user/
syncwidget.path = /etc/syncwidget/
syncwidget.files = com.meego.msyncd
gschemas.path = /usr/share/glib-2.0/schemas
gschemas.files = gschemas/com.meego.msyncd.gschema.xml

INSTALLS += target \
    syncwidget \
    service \
    gschemas
