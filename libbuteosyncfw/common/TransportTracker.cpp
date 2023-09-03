/*
 * This file is part of buteo-syncfw package
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 *               2019 Updated to use bluez5 by deloptes@gmail.com
 *
 * Contact: Sateesh Kavuri <sateesh.kavuri@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */
#include "TransportTracker.h"
#if __USBMODED__
#include "USBModedProxy.h"
#endif
#include <QtCore>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include "NetworkManager.h"
#else
#include <QNetworkInformation>
#endif

#include "LogMacros.h"
#include <QMutexLocker>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>

using namespace Buteo;

TransportTracker::TransportTracker(QObject *aParent) :
    QObject(aParent),
    iUSBProxy(0),
    iInternet(0),
    iSystemBus(QDBusConnection::systemBus())
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    iTransportStates[Sync::CONNECTIVITY_USB] = false;
    iTransportStates[Sync::CONNECTIVITY_BT] = false;
    iTransportStates[Sync::CONNECTIVITY_INTERNET] = false;

#if __USBMODED__
    // USB
    iUSBProxy = new USBModedProxy(this);
    if (!iUSBProxy->isValid()) {
        qCCritical(lcButeoCore) << "Failed to connect to USB moded D-Bus interface";
        delete iUSBProxy;
        iUSBProxy = nullptr;
    } else {
        QObject::connect(iUSBProxy, SIGNAL(usbConnection(bool)), this,
                         SLOT(onUsbStateChanged(bool)));
        iTransportStates[Sync::CONNECTIVITY_USB] =
            iUSBProxy->isUSBConnected();
    }
#endif

#ifdef HAVE_BLUEZ_5
    // BT
    qDBusRegisterMetaType <InterfacesMap> ();
    qDBusRegisterMetaType <ObjectsMap> ();

    // listen for added interfaces
    if (!iSystemBus.connect(BT::BLUEZ_DEST,
                     QString("/"),
                     BT::BLUEZ_MANAGER_INTERFACE,
                     BT::INTERFACESADDED,
                     this,
                     SLOT(onBtInterfacesAdded(const QDBusObjectPath &, const InterfacesMap &)))) {
        qCWarning(lcButeoCore) << "Failed to connect InterfacesAdded signal";
    }

    if (!iSystemBus.connect(BT::BLUEZ_DEST,
                     QString("/"),
                     BT::BLUEZ_MANAGER_INTERFACE,
                     BT::INTERFACESREMOVED,
                     this,
                     SLOT(onBtInterfacesRemoved(const QDBusObjectPath &, const QStringList &)))) {
        qCWarning(lcButeoCore) << "Failed to connect InterfacesRemoved signal";
    }

    // get the initial state
    if (btConnectivityStatus()) {
        if (!iSystemBus.connect(BT::BLUEZ_DEST,
                iDefaultBtAdapter,
                BT::BLUEZ_PROPERTIES_INTERFACE,
                BT::PROPERTIESCHANGED,
                this,
                SLOT(onBtStateChanged(const QString &, const QVariantMap &, const QStringList &)))) {
            qCWarning(lcButeoCore) << "Failed to connect PropertiesChanged signal";
        }
        // Set the bluetooth state to on
        iTransportStates[Sync::CONNECTIVITY_BT] = true;
    } else {
        qCWarning(lcButeoCore) << "The BT adapter is powered off or missing";
    }
#endif
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    // Internet
    // @todo: enable when internet state is reported correctly.
    iInternet = new NetworkManager(this);
    iTransportStates[Sync::CONNECTIVITY_INTERNET] =
            iInternet->isOnline();
    connect(iInternet,
            SIGNAL(statusChanged(bool, Sync::InternetConnectionType)),
            SLOT(onInternetStateChanged(bool, Sync::InternetConnectionType)) /*, Qt::QueuedConnection*/);
#else
    QNetworkInformation* iInternet = QNetworkInformation::instance();
    connect(iInternet, &QNetworkInformation::transportMediumChanged, this, &TransportTracker::onInternetStateChanged);
#endif
}

TransportTracker::~TransportTracker()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
}

bool TransportTracker::isConnectivityAvailable(Sync::ConnectivityType aType) const
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    QMutexLocker locker(&iMutex);

    return iTransportStates[aType];
}

void TransportTracker::onUsbStateChanged(bool aConnected)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoCore) << "USB state changed:" << aConnected;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    m_connected = aConnected;
    m_aType = QNetworkInformation::TransportMedium::Ethernet;
    updateState();
#else
    updateState(Sync::CONNECTIVITY_USB, aConnected);
#endif
}

#ifdef HAVE_BLUEZ_5
void TransportTracker::onBtStateChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    Q_UNUSED(invalidated);

    if (interface == BT::BLUEZ_ADAPTER_INTERFACE) {
        for (QVariantMap::const_iterator i = changed.begin(); i != changed.end(); ++i) {
            if (i.key() == "Powered") {
                bool btOn = i.value().toBool();
                qCInfo(lcButeoCore) << "BT power state " << btOn;
                updateState(Sync::CONNECTIVITY_BT, btOn);
            }
        }
    }
}

void TransportTracker::onBtInterfacesAdded(const QDBusObjectPath &path, const InterfacesMap &interfaces)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    for (InterfacesMap::const_iterator i = interfaces.cbegin(); i != interfaces.cend(); ++i) {
        if (i.key() == BT::BLUEZ_ADAPTER_INTERFACE) {

            // do not process other interfaces after default one was selected
            if (!iDefaultBtAdapter.isEmpty())
                break;

            iDefaultBtAdapter = path.path();
            qCDebug(lcButeoCore) << BT::BLUEZ_ADAPTER_INTERFACE << "interface" << iDefaultBtAdapter;

            QDBusInterface adapter(BT::BLUEZ_DEST,
                    iDefaultBtAdapter,
                    BT::BLUEZ_ADAPTER_INTERFACE,
                    iSystemBus);

            if (!iSystemBus.connect(BT::BLUEZ_DEST,
                    iDefaultBtAdapter,
                    BT::BLUEZ_PROPERTIES_INTERFACE,
                    BT::PROPERTIESCHANGED,
                    this,
                    SLOT(onBtStateChanged(const QString &, const QVariantMap &, const QStringList &)))) {
                qCWarning(lcButeoCore) << "Failed to connect PropertiesChanged signal";

            }

            if (adapter.isValid()) {
                updateState(Sync::CONNECTIVITY_BT, adapter.property("Powered").toBool());
                qCInfo(lcButeoCore) << "BT state changed" << adapter.property("Powered").toBool();
            }
        }
    }
}

void TransportTracker::onBtInterfacesRemoved(const QDBusObjectPath &path, const QStringList &interfaces)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    for (QStringList::const_iterator i = interfaces.cbegin(); i != interfaces.cend(); ++i) {
        if (*i == BT::BLUEZ_ADAPTER_INTERFACE) {

            if (path.path() != iDefaultBtAdapter)
                continue;

            qCDebug(lcButeoCore) << "DBus adapter path: " << iDefaultBtAdapter ;

            if (!iSystemBus.disconnect(BT::BLUEZ_DEST,
                    iDefaultBtAdapter,
                    BT::BLUEZ_PROPERTIES_INTERFACE,
                    BT::PROPERTIESCHANGED,
                    this,
                    SLOT(onBtStateChanged(const QString &, const QVariantMap &, const QStringList &)))) {
                qCWarning(lcButeoCore) << "Failed to disconnect PropertiesChanged signal";
            } else {
                qCDebug(lcButeoCore) << "'org.bluez.Adapter1' interface removed from " << path.path();
            }

            iDefaultBtAdapter = QString();

            break;
        }
    }
}
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void TransportTracker::onReachabilityChanged(QNetworkInformation::Reachability newReachability)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    bool connected;
    if(newReachability == QNetworkInformation::Reachability::Online) {
        connected = true;
    } else {
        connected = false;
    }
    if(connected != m_connected) {
        m_connected = connected;
        updateState();
    }
}

void TransportTracker::onInternetStateChanged(QNetworkInformation::TransportMedium aType) {
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if(aType != m_aType) {
        m_aType = aType;
        updateState();
    }
}

void TransportTracker::updateState() {
    emit connectivityStateChanged(m_aType, m_connected);
}

#else
void TransportTracker::onInternetStateChanged(bool aConnected, Sync::InternetConnectionType aType)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoCore) << "Internet state changed:" << aConnected;
    updateState(Sync::CONNECTIVITY_INTERNET, aConnected);
    emit networkStateChanged(aConnected, aType);
}

void TransportTracker::updateState(Sync::ConnectivityType aType, bool aState)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);


    bool oldState = false;
    {
        QMutexLocker locker(&iMutex);
        oldState = iTransportStates[aType];
        iTransportStates[aType] = aState;
    }
    if (oldState != aState) {
        if (aType != Sync::CONNECTIVITY_INTERNET) {
            emit connectivityStateChanged(aType, aState);
        }
    }
}
#endif
#ifdef HAVE_BLUEZ_5
bool TransportTracker::btConnectivityStatus()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    QDBusInterface  manager(BT::BLUEZ_DEST,
            QString("/"),
            BT::BLUEZ_MANAGER_INTERFACE,
            iSystemBus);

    QDBusReply<ObjectsMap> reply = manager.call(BT::GETMANAGEDOBJECTS);
    if (!reply.isValid()) {
        qCWarning(lcButeoCore) << "Failed to connect BT ObjectManager: " << reply.error().message() ;
        return false;
    }

    ObjectsMap objects = reply.value();
    for (ObjectsMap::iterator i = objects.begin(); i != objects.end(); ++i) {

        InterfacesMap ifaces = i.value();
        for (InterfacesMap::const_iterator j = ifaces.cbegin(); j != ifaces.cend(); ++j) {

            if (j.key() == BT::BLUEZ_ADAPTER_INTERFACE) {
                if (iDefaultBtAdapter.isEmpty() || iDefaultBtAdapter != i.key().path()) {
                    iDefaultBtAdapter = i.key().path();
                    qCDebug(lcButeoCore) << "Using adapter path: " << iDefaultBtAdapter;
                }
                QDBusInterface adapter(BT::BLUEZ_DEST,
                        iDefaultBtAdapter,
                        BT::BLUEZ_ADAPTER_INTERFACE,
                        iSystemBus);

                return adapter.property("Powered").toBool(); // use first adapter
            }
        }
    }

    return false;
}
#endif
