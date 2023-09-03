isEmpty(TESTS_COMMON_PRI_INCLUDED) {
TESTS_COMMON_PRI_INCLUDED = 1

tests_subdir = $$relative_path($$dirname(_PRO_FILE_), $${PWD})
tests_subdir_r = $$relative_path($${PWD}, $$dirname(_PRO_FILE_))

QT += testlib \
    core \
    dbus \
    xml \
    dbus \
    network \
    sql

QT -= gui

CONFIG += link_pkgconfig link_prl

PKGCONFIG += dbus-1

LIBS += -L$${OUT_PWD}/$${tests_subdir_r}/../../libbuteosyncfw

PKGCONFIG += libsignon-qt$${QT_MAJOR_VERSION} accounts-qt$${QT_MAJOR_VERSION}
LIBS += -lbuteosyncfw$${QT_MAJOR_VERSION}

# This is needed to avoid adding the /usr/lib link directory before the
# newer version in the present directories
QMAKE_LIBDIR_QT = $${OUT_PWD}/$${tests_subdir_r}/../../libbuteosyncfw

DEFINES += SYNCFW_UNIT_TESTS

INCLUDEPATH = \
    $${PWD} \
    $${PWD}/../.. \
    $${PWD}/../../libbuteosyncfw/clientfw \
    $${PWD}/../../libbuteosyncfw/common \
    $${PWD}/../../libbuteosyncfw/pluginmgr \
    $${PWD}/../../libbuteosyncfw/profile \
    $${PWD}/../../msyncd \

CONFIG -= depend_includepath
DEPENDPATH = \
    $${PWD}/../../libbuteosyncfw/clientfw \
    $${PWD}/../../libbuteosyncfw/common \
    $${PWD}/../../libbuteosyncfw/pluginmgr \
    $${PWD}/../../libbuteosyncfw/profile \
    $${PWD}/../../msyncd \

INSTALL_TESTDIR = /opt/tests/buteo-syncfw
INSTALL_TESTDATADIR = $${INSTALL_TESTDIR}/data

}
