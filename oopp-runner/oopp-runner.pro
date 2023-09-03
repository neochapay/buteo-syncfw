TEMPLATE = app
TARGET = buteo-oopp-runner

QT += dbus network
QT -= gui

INCLUDEPATH += $$PWD \
    ../libbuteosyncfw/pluginmgr \
    ../libbuteosyncfw/clientfw \
    ../libbuteosyncfw/common \
    ../libbuteosyncfw/profile

LIBS += -lbuteosyncfw$${QT_MAJOR_VERSION}
LIBS += -L../libbuteosyncfw

HEADERS += ButeoPluginIfaceAdaptor.h \
           PluginCbImpl.h \
           PluginServiceObj.h

SOURCES += ButeoPluginIfaceAdaptor.cpp \
           PluginCbImpl.cpp \
           PluginServiceObj.cpp \
           main.cpp

target.path = /usr/libexec/
INSTALLS += target
