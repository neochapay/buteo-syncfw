TEMPLATE = lib
TARGET = hdummy-server
DEPENDPATH += .
INCLUDEPATH += . \
    ../../.. \
    ../../../libbuteosyncfw/common \
    ../../../libbuteosyncfw/pluginmgr \
    ../../../libbuteosyncfw/profile

CONFIG += plugin
QT -= gui
QT += network

#input
HEADERS += DummyServer.h

SOURCES += DummyServer.cpp

#clean
QMAKE_CLEAN += $(TARGET) $(TARGET0) $(TARGET1) $(TARGET2)
QMAKE_CLEAN += $(OBJECTS_DIR)/moc_*

target.path = /opt/tests/buteo-syncfw/
INSTALLS += target
