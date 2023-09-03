TEMPLATE = lib
TARGET = hdummy-client
DEPENDPATH += .
INCLUDEPATH += . \
    ../../.. \
    ../../../libbuteosyncfw/common \
    ../../../libbuteosyncfw/pluginmgr \
    ../../../libbuteosyncfw/profile

QT -= gui
QT += network
CONFIG += plugin

HEADERS += DummyClient.h

SOURCES += DummyClient.cpp

#clean
QMAKE_CLEAN += $(TARGET) $(TARGET0) $(TARGET1) $(TARGET2)
QMAKE_CLEAN += $(OBJECTS_DIR)/moc_*

target.path = /opt/tests/buteo-syncfw
INSTALLS += target
