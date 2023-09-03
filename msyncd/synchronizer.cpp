/*
 * This file is part of buteo-syncfw package
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2014-2019 Jolla Ltd.
 * Copyright (C) 2020 Open Mobile Platform LLC.
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
#include <gio/gio.h>
#include "synchronizer.h"
#include "SyncDBusAdaptor.h"
#include "SyncSession.h"
#include "ClientPluginRunner.h"
#include "ServerPluginRunner.h"
#include "AccountsHelper.h"
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include "NetworkManager.h"
#endif
#include "TransportTracker.h"
#include "ServerActivator.h"

#include "SyncCommonDefs.h"
#include "StoragePlugin.h"
#include "SyncLog.h"
#include "ClientPlugin.h"
#include "ServerPlugin.h"
#include "ProfileFactory.h"
#include "ProfileEngineDefs.h"
#include "LogMacros.h"
#include "BtHelper.h"

#ifdef HAS_MCE
#include <qmcebatterystatus.h>
#include <qmcepowersavemode.h>
#endif
#include <QtDebug>
#include <fcntl.h>
#include <termios.h>

using namespace Buteo;

static const QString SYNC_DBUS_OBJECT = "/synchronizer";
static const QString SYNC_DBUS_SERVICE = "com.meego.msyncd";
static const QString BT_PROPERTIES_NAME = "Name";

class Buteo::BatteryInfo
{
#ifdef HAS_MCE
public:
    bool isLowPower()
    {
        return (iBatteryStatus.valid() &&
                iBatteryStatus.status() <= QMceBatteryStatus::Low);
    }
    bool inPowerSaveMode()
    {
        return (iPowerSaveMode.valid() &&
                iPowerSaveMode.active());
    }
private:
    QMcePowerSaveMode iPowerSaveMode;
    QMceBatteryStatus iBatteryStatus;
#else
public:
    bool isLowPower()
    {
        return false;
    }
    bool inPowerSaveMode()
    {
        return false;
    }
#endif
};

Synchronizer::Synchronizer(QCoreApplication *aApplication)
    : iNetworkManager(0)
    , iSyncScheduler(0)
    , iSyncBackup(0)
    , iTransportTracker(0)
    , iServerActivator(0)
    , iAccounts(0)
    , iClosing(false)
    , iSOCEnabled(false)
    , iSyncUIInterface(nullptr)
    , iBatteryInfo(new BatteryInfo)
{
    iSettings = g_settings_new_with_path("com.meego.msyncd", "/com/meego/msyncd/");
    FUNCTION_CALL_TRACE(lcButeoTrace);
    this->setParent(aApplication);

    iProfileChangeTriggerTimer.setSingleShot(true);
    connect(&iProfileChangeTriggerTimer, &QTimer::timeout,
            this, &Synchronizer::profileChangeTriggerTimeout);
}

Synchronizer::~Synchronizer()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    delete iSyncUIInterface;
    iSyncUIInterface = nullptr;
    g_object_unref(iSettings);
    delete iBatteryInfo;
}

bool Synchronizer::initialize()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Starting msyncd";

    // Create a D-Bus adaptor. It will get deleted when the Synchronizer is
    // deleted.
    new SyncDBusAdaptor(this);

    // Register our object on the session bus and expose interface to others.
    QDBusConnection dbus = QDBusConnection::sessionBus();
    if (!dbus.registerObject(SYNC_DBUS_OBJECT, this) ||
            !dbus.registerService(SYNC_DBUS_SERVICE)) {
        qCCritical(lcButeoMsyncd) << "Failed to register to D-Bus (D-Bus not started or msyncd already running?), aborting start";
        return false;
    } else {
        qCDebug(lcButeoMsyncd) << "Registered to D-Bus";
    } // else ok

    connect(this, SIGNAL(syncStatus(QString, int, QString, int)),
            this, SLOT(slotSyncStatus(QString, int, QString, int)),
            Qt::QueuedConnection);

    // use queued connection because the profile will be stored after the signal
    connect(&iProfileManager, SIGNAL(signalProfileChanged(QString, int, QString)),
            this, SLOT(slotProfileChanged(QString, int, QString)), Qt::QueuedConnection);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    iNetworkManager = QNetworkInformation::instance();
#else
    iNetworkManager = new NetworkManager(this);
#endif
    iTransportTracker = new TransportTracker(this);

    iServerActivator = new ServerActivator(iProfileManager,
                                           *iTransportTracker, this);
    connect(iTransportTracker,
            SIGNAL(networkStateChanged(bool, Sync::InternetConnectionType)),
            SLOT(onNetworkStateChanged(bool, Sync::InternetConnectionType)));

    // Initialize account manager.
    iAccounts = new AccountsHelper(iProfileManager, this); // Deleted with parent.
    connect(iAccounts, SIGNAL(enableSOC(QString)),
            this, SLOT(enableSOCSlot(QString)),
            Qt::QueuedConnection);
    connect(iAccounts, SIGNAL(scheduleUpdated(QString)),
            this, SLOT(reschedule(QString)),
            Qt::QueuedConnection);
    connect(iAccounts, SIGNAL(removeProfile(QString)),
            this, SLOT(removeProfile(QString)),
            Qt::QueuedConnection);
    connect(iAccounts, SIGNAL(removeScheduledSync(QString)),
            this, SLOT(removeScheduledSync(QString)),
            Qt::QueuedConnection);
    connect(this, SIGNAL(storageReleased()),
            this, SLOT(onStorageReleased()), Qt::QueuedConnection);

    startServers();

    // For Backup/restore handling
    iSyncBackup = new SyncBackup();

    // Initialize scheduler
    initializeScheduler();

    // Connect backup signals after the scheduler has been initialized
    connect(iSyncBackup, SIGNAL(startBackup()), this, SLOT(backupStarts()));
    connect(iSyncBackup, SIGNAL(backupDone()), this, SLOT(backupFinished()));
    connect(iSyncBackup, SIGNAL(startRestore()), this, SLOT(restoreStarts()));
    connect(iSyncBackup, SIGNAL(restoreDone()), this, SLOT(restoreFinished()));

    //For Sync On Change
    // enable SOC for contacts only as of now
    QHash<QString, QList<SyncProfile *> > aSOCStorageMap;
    //TODO can we do away with hard coding storage (plug-in) names, in other words do they
    //have to be well known this way
    QList<SyncProfile *> SOCProfiles = iProfileManager.getSOCProfilesForStorage("./contacts");
    if (SOCProfiles.count()) {
        // TODO Come up with a scheme to avoid hard-coding, this is not done just here
        // but also for storage plug-ins in onStorageAquired
        aSOCStorageMap["hcontacts"] = SOCProfiles;
        QStringList aFailedStorages;
        bool isSOCEnabled = iSyncOnChange.enable(aSOCStorageMap, &iSyncOnChangeScheduler,
                                                 &iPluginManager, aFailedStorages);
        if (!isSOCEnabled) {
            foreach (const QString &aStorageName, aFailedStorages) {
                qCCritical(lcButeoMsyncd) << "Sync on change couldn't be enabled for storage" << aStorageName;
            }
        } else {
            QObject::connect(&iSyncOnChangeScheduler, SIGNAL(syncNow(QString)),
                             this, SLOT(startScheduledSync(QString)),
                             Qt::QueuedConnection);
            iSOCEnabled = true;
        }
    } else {
        qCDebug(lcButeoMsyncd) << "No profiles interested in SOC";
    }
    return true;
}

void Synchronizer::enableSOCSlot(const QString &aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    SyncProfile *profile = iProfileManager.syncProfile(aProfileName);
    if (profile && profile->isSOCProfile()) {
        if (iSOCEnabled) {
            iSyncOnChange.addProfile("hcontacts", profile);
        } else {
            QHash<QString, QList<SyncProfile *> > aSOCStorageMap;
            QList<SyncProfile *> SOCProfiles;
            SOCProfiles.append(profile);
            aSOCStorageMap["hcontacts"] = SOCProfiles;
            QStringList aFailedStorages;
            if (iSyncOnChange.enable(aSOCStorageMap, &iSyncOnChangeScheduler, &iPluginManager, aFailedStorages)) {
                QObject::connect(&iSyncOnChangeScheduler, SIGNAL(syncNow(const QString &)),
                                 this, SLOT(startScheduledSync(const QString &)),
                                 Qt::QueuedConnection);
                iSOCEnabled = true;
                qCDebug(lcButeoMsyncd) << "Sync on change enabled for profile" << aProfileName;
            } else {
                qCCritical(lcButeoMsyncd) << "Sync on change couldn't be enabled for profile" << aProfileName;
            }
        }
    } else {
        delete profile;
    }
}

void Synchronizer::close()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Stopping msyncd";

    iClosing = true;

    // Stop running sessions
    if (iSOCEnabled) {
        iSyncOnChange.disable();
    }

    QList<SyncSession *> sessions = iActiveSessions.values();
    foreach (SyncSession *session, sessions) {
        if (session != 0) {
            session->stop();
        }
    }
    qDeleteAll(sessions);
    iActiveSessions.clear();

    stopServers();

    delete iSyncScheduler;
    iSyncScheduler = 0;

    delete iSyncBackup;
    iSyncBackup = 0;

    // Unregister from D-Bus.
    QDBusConnection dbus = QDBusConnection::sessionBus();
    dbus.unregisterObject(SYNC_DBUS_OBJECT);
    if (!dbus.unregisterService(SYNC_DBUS_SERVICE)) {
        qCWarning(lcButeoMsyncd) << "Failed to unregister from D-Bus";
    } else {
        qCDebug(lcButeoMsyncd) << "Unregistered from D-Bus";
    }
}

bool Synchronizer::startSync(QString aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    // Manually triggered sync.
    return startSync(aProfileName, false);
}

bool Synchronizer::startScheduledSync(QString aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    SyncProfile *profile = iProfileManager.syncProfile(aProfileName);

    // All scheduled syncs are online syncs
    // Add this to the waiting online syncs and it will be started when we
    // receive a session connection status from the NetworkManager
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool accept = acceptScheduledSync(profile);
#else
    bool accept = acceptScheduledSync(iNetworkManager->isOnline(), iNetworkManager->connectionType(), profile);
#endif
    if (accept) {
        /* Ensure that current time is compatible with sync schedule.
           The Background process may have started a sync in a period
           where sync is disabled due to delayed interval wake up. */
        bool wrongTime = (profile && !profile->syncSchedule().isSyncScheduled(QDateTime::currentDateTime(),
                                                                              profile->lastSuccessfulSyncTime()));
        if (wrongTime) {
            qCDebug(lcButeoMsyncd) << "Woken up of" << aProfileName << "in a disabled period, not starting sync.";
            if (iSyncScheduler) {
                // can be null if in backup/restore state.
                iSyncScheduler->syncStatusChanged(aProfileName,
                                                  Sync::SYNC_NOTPOSSIBLE,
                                                  QLatin1String("Sync cancelled due to wrong wake up"),
                                                  Buteo::SyncResults::ABORTED);
            }
        } else {
            qCDebug(lcButeoMsyncd) << "Scheduled sync of" << aProfileName << "accepted with current connection type" <<
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                      iNetworkManager->transportMedium();
#else
                      iNetworkManager->connectionType();
#endif
            startSync(aProfileName, true);
        }
    } else {
        qCInfo(lcButeoMsyncd) << "Wait for internet connection:" << aProfileName;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        if (iNetworkManager->reachability() == QNetworkInformation::Reachability::Online) {
#else
        if (iNetworkManager->isOnline()) {
#endif
            // see acceptScheduledSync() for the determination of whether the connection type is allowed for sync operations.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            qCInfo(lcButeoMsyncd) << "Connection" << iNetworkManager->transportMedium() <<
#else
            qCInfo(lcButeoMsyncd) << "Connection" << iNetworkManager->connectionType() <<
#endif
                     "is of disallowed type. The sync will be postponed until an allowed connection is available.";
        } else {
            qCInfo(lcButeoMsyncd) << "Device offline. Wait for internet connection.";
        }

        if (!iWaitingOnlineSyncs.contains(aProfileName)) {
            iWaitingOnlineSyncs.append(aProfileName);
        }

        qCDebug(lcButeoMsyncd) << "Marking" << aProfileName << "sync as NOTPOSSIBLE due to connectivity status";
        if (iSyncScheduler) {
            // can be null if in backup/restore state.
            iSyncScheduler->syncStatusChanged(aProfileName,
                                              Sync::SYNC_NOTPOSSIBLE,
                                              QLatin1String("No internet connectivity or connectivity type restricted for sync"),
                                              Buteo::SyncResults::OFFLINE_MODE);
        }
    }
    delete profile;
    return true;
}

bool Synchronizer::setSyncSchedule(QString aProfileId, QString aScheduleAsXml)
{
    bool status = false;
    if (iProfileManager.setSyncSchedule(aProfileId, aScheduleAsXml)) {
        reschedule(aProfileId);
        status = true;
    }
    return status;
}

bool Synchronizer::saveSyncResults(QString aProfileId, QString aSyncResults)
{
    QDomDocument doc;
    bool status = false;
    if (doc.setContent(aSyncResults, true)) {
        Buteo::SyncResults results(doc.documentElement());
        status = iProfileManager.saveSyncResults(aProfileId, results);
    } else {
        qCCritical(lcButeoMsyncd) << "Invalid Profile Xml Received from msyncd";
    }

    return status;
}

QString Synchronizer::createSyncProfileForAccount(uint aAccountId)
{
    iAccounts->createProfileForAccount(aAccountId);
    // There could be profiles created for every service. There is no
    // point to give back a profile name.
    return QString();
}

bool Synchronizer::startSync(const QString &aProfileName, bool aScheduled)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    bool success = false;

    if (isBackupRestoreInProgress()) {
        SyncResults syncResults(QDateTime::currentDateTime(), SyncResults::SYNC_RESULT_FAILED,
                                Buteo::SyncResults::BACKUP_IN_PROGRESS);
        iProfileManager.saveSyncResults(aProfileName, syncResults);
        emit syncStatus(aProfileName, Sync::SYNC_NOTPOSSIBLE, "Backup in progress, cannot start sync",
                        Buteo::SyncResults::BACKUP_IN_PROGRESS);
        return success;
    }

    qCDebug(lcButeoMsyncd) << "Start sync requested for profile:" << aProfileName;

    // This function can be called from a client app as manual sync:
    // If we receive a manual sync to a profile that is peding to sync due a
    // data change we can remove it from the iSyncOnChangeScheduler, to avoid a
    // second sync.
    iSyncOnChangeScheduler.removeProfile(aProfileName);

    // Do the same if the profile is pending sync due to a profile change.
    for (int i = iProfileChangeTriggerQueue.size() - 1; i >= 0; --i) {
        if (iProfileChangeTriggerQueue[i].first == aProfileName) {
            qCDebug(lcButeoMsyncd) << "Removing queued profile change sync due to sync trigger:" << aProfileName;
            iProfileChangeTriggerQueue.removeAt(i);
        }
    }

    if (iActiveSessions.contains(aProfileName)) {
        qCDebug(lcButeoMsyncd) << "Sync already in progress";
        return true;
    } else if (iSyncQueue.contains(aProfileName)) {
        qCDebug(lcButeoMsyncd) << "Sync request already in queue";
        emit syncStatus(aProfileName, Sync::SYNC_QUEUED, "", 0);
        return true;
    } else if (!aScheduled && iWaitingOnlineSyncs.contains(aProfileName)) {
        // Manual sync is allowed to happen in any kind of connection
        // if sync is not scheduled remove it from iWaitingOnlineSyncs to avoid
        // sync it twice later
        iWaitingOnlineSyncs.removeOne(aProfileName);
        qCDebug(lcButeoMsyncd) << "Removing" << aProfileName << "from online waiting list.";
    }

    SyncProfile *profile = iProfileManager.syncProfile(aProfileName);
    if (!profile) {
        qCWarning(lcButeoMsyncd) << "Profile not found";
        SyncResults syncResults(QDateTime::currentDateTime(), SyncResults::SYNC_RESULT_FAILED,
                                Buteo::SyncResults::INTERNAL_ERROR);
        iProfileManager.saveSyncResults(aProfileName, syncResults);
        emit syncStatus(aProfileName, Sync::SYNC_ERROR, "Internal Error", Buteo::SyncResults::INTERNAL_ERROR);
        return false;
    } else if (false == profile->isEnabled()) {
        qCWarning(lcButeoMsyncd) << "Profile is disabled, not starting sync";
        SyncResults syncResults(QDateTime::currentDateTime(), SyncResults::SYNC_RESULT_FAILED,
                                Buteo::SyncResults::INTERNAL_ERROR);
        iProfileManager.saveSyncResults(aProfileName, syncResults);
        emit syncStatus(aProfileName, Sync::SYNC_ERROR, "Internal Error", Buteo::SyncResults::INTERNAL_ERROR);
        delete profile;
        return false;
    }

    SyncSession *session = new SyncSession(profile, this);
    session->setScheduled(aScheduled);

    if (profile->clientProfile()
            && clientProfileActive(profile->clientProfile()->name())) {
        qCDebug(lcButeoMsyncd) << "Sync request of the same type in progress, adding request to the sync queue";
        iSyncQueue.enqueue(session);
        emit syncStatus(aProfileName, Sync::SYNC_QUEUED, "", 0);
        return false;
    }

    // @todo: Complete profile with data from account manager.
    //iAccounts->addAccountData(*profile);

    if (!profile->isValid()) {
        qCWarning(lcButeoMsyncd) << "Profile is not valid";
        session->setFailureResult(SyncResults::SYNC_RESULT_FAILED, Buteo::SyncResults::INTERNAL_ERROR);
        emit syncStatus(aProfileName, Sync::SYNC_ERROR, "Internal Error", Buteo::SyncResults::INTERNAL_ERROR);
    } else if (aScheduled && iBatteryInfo->isLowPower()) {
        qCWarning(lcButeoMsyncd) << "Low power, scheduled sync for profile" << aProfileName << "aborted";
        session->setFailureResult(SyncResults::SYNC_RESULT_FAILED, Buteo::SyncResults::LOW_BATTERY_POWER);
        emit syncStatus(aProfileName, Sync::SYNC_ERROR, "Low battery", Buteo::SyncResults::LOW_BATTERY_POWER);
    } else if (aScheduled && iBatteryInfo->inPowerSaveMode()) {
        qCWarning(lcButeoMsyncd) << "Power save mode active, scheduled sync for profile" << aProfileName << "aborted";
        session->setFailureResult(SyncResults::SYNC_RESULT_FAILED, Buteo::SyncResults::POWER_SAVING_MODE);
        emit syncStatus(aProfileName, Sync::SYNC_ERROR, "Power Save Mode active", Buteo::SyncResults::POWER_SAVING_MODE);
    } else if (!session->reserveStorages(&iStorageBooker)) {
        qCDebug(lcButeoMsyncd) << "Needed storage(s) already in use, queuing sync request";
        iSyncQueue.enqueue(session);
        emit syncStatus(aProfileName, Sync::SYNC_QUEUED, "", 0);
        success = true;
    } else {
        // Sync can be started now.
        success = startSyncNow(session);
        if (success) {
            emit syncStatus(aProfileName, Sync::SYNC_STARTED, "", 0);
        } else {
            qCWarning(lcButeoMsyncd) << "Internal error, unable to start sync session for profile:" << aProfileName;
            session->setFailureResult(SyncResults::SYNC_RESULT_FAILED, Buteo::SyncResults::INTERNAL_ERROR);
            emit syncStatus(aProfileName, Sync::SYNC_ERROR, "Internal Error", Buteo::SyncResults::INTERNAL_ERROR);
        }
    }

    if (!success) {
        cleanupSession(session, Sync::SYNC_ERROR);
    }

    return success;
}

bool Synchronizer::startSyncNow(SyncSession *aSession)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (!aSession || isBackupRestoreInProgress()) {
        qCWarning(lcButeoMsyncd) << "Session is null || backup in progress";
        return false;
    }

    SyncProfile *profile = aSession->profile();
    if (!profile) {
        qCWarning(lcButeoMsyncd) << "Profile in session is null";
        return false;
    }

    qCDebug(lcButeoMsyncd) << "Starting sync with profile" <<  aSession->profileName();

    Profile *clientProfile = profile->clientProfile();
    if (clientProfile == 0) {
        qCWarning(lcButeoMsyncd) << "Could not find client sub-profile";
        return false;
    }

    qCDebug(lcButeoMsyncd) << "Disable sync on change:" << iSOCEnabled << profile->isSOCProfile();
    //As sync is ongoing, disable sync on change for now, we can query later if
    //there are changes.
    if (iSOCEnabled) {
        if (profile->isSOCProfile()) {
            iSyncOnChange.disable();
        } else {
            iSyncOnChange.disableNext();
        }
    }

    iProfileManager.addRetriesInfo(profile);

    PluginRunner *pluginRunner = new ClientPluginRunner(
        clientProfile->name(), aSession->profile(), &iPluginManager, this,
        this);
    aSession->setPluginRunner(pluginRunner, true);
    if (!pluginRunner->init()) {
        qCWarning(lcButeoMsyncd) << "Failed to initialize client plug-in runner";
        return false;
    }

    // Relay connectivity state change signal to plug-in runner.
    connect(iTransportTracker, SIGNAL(connectivityStateChanged(Sync::ConnectivityType, bool)),
            pluginRunner, SIGNAL(connectivityStateChanged(Sync::ConnectivityType, bool)));

    qCDebug(lcButeoMsyncd) << "Client plug-in runner initialized";

    // Connect signals from sync session.
    connect(aSession, SIGNAL(transferProgress(const QString &,
                                              Sync::TransferDatabase, Sync::TransferType, const QString &, int)),
            this, SLOT(onTransferProgress(const QString &,
                                          Sync::TransferDatabase, Sync::TransferType, const QString &, int)));

    connect(aSession, SIGNAL(storageAccquired(const QString &, const QString &)),
            this, SLOT(onStorageAccquired(const QString &, const QString &)));
    connect(aSession, SIGNAL(syncProgressDetail(const QString &, int)),
            this, SLOT(onSyncProgressDetail(const QString &, int)));

    connect(aSession, &SyncSession::finished, this, &Synchronizer::onSessionFinished);

    if (aSession->start()) {
        // Get the DBUS interface for sync-UI.
        qCDebug(lcButeoMsyncd) << "sync-ui dbus interface is getting called";
        if (aSession->isScheduled() && !profile->isHidden()) {
            if (iSyncUIInterface == nullptr) {
                qCDebug(lcButeoMsyncd) << "iSyncUIInterface is Null";
                iSyncUIInterface = new QDBusInterface("com.nokia.syncui", "/org/maemo/m",
                                                      "com.nokia.MApplicationIf", QDBusConnection::sessionBus());
            } else if (!iSyncUIInterface->isValid()) {
                qCDebug(lcButeoMsyncd) << "iSyncUIInterface is not valid";
                delete iSyncUIInterface;
                iSyncUIInterface = nullptr;
                iSyncUIInterface = new QDBusInterface("com.nokia.syncui", "/org/maemo/m",
                                                      "com.nokia.MApplicationIf", QDBusConnection::sessionBus());
            }
            //calling launch with argument list
            QStringList list;
            list.append("launching");
            QList<QVariant> argumentList;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            argumentList << QVariant::fromValue(list);
#else
            argumentList << qVariantFromValue(list);
#endif
            iSyncUIInterface->asyncCallWithArgumentList(QLatin1String("launch"), argumentList);
        }

        qCDebug(lcButeoMsyncd) << "Sync session started";
        iActiveSessions.insert(aSession->profileName(), aSession);
    } else {
        qCWarning(lcButeoMsyncd) << "Failed to start sync session";
        return false;
    }

    return true;
}

void Synchronizer::onSessionFinished(const QString &aProfileName,
                                     Sync::SyncStatus aStatus, const QString &aMessage, SyncResults::MinorCode aErrorCode)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Session finished:" << aProfileName << ", status:" << aStatus;

    if (iActiveSessions.contains(aProfileName)) {
        SyncSession *session = iActiveSessions[aProfileName];
        if (session) {
            switch (aStatus) {
            case Sync::SYNC_DONE: {
                bool enabledUpdated = false, visibleUpdated = false;
                QMap<QString, bool> storageMap = session->getStorageMap();
                SyncProfile *sessionProf = session->profile();
                iProfileManager.enableStorages(*sessionProf, storageMap, &enabledUpdated);

                // If caps have not been modified, i.e. fetched from the remote device yet, set
                // enabled storages also visible. If caps have been modified, we must not touch visibility anymore
                if (sessionProf->boolKey(KEY_CAPS_MODIFIED) == false) {
                    iProfileManager.setStoragesVisible(*sessionProf, storageMap, &visibleUpdated);
                }

                if (enabledUpdated || visibleUpdated) {
                    iProfileManager.updateProfile(*sessionProf);
                }
                iProfileManager.retriesDone(sessionProf->name());
                break;
            }

            case Sync::SYNC_ABORTED:
            case Sync::SYNC_CANCELLED: {
                session->setFailureResult(SyncResults::SYNC_RESULT_CANCELLED, Buteo::SyncResults::ABORTED);
                if (session->isProfileCreated()) {
                    iProfileManager.removeProfile(session->profileName());
                }
                break;
            }
            case Sync::SYNC_ERROR: {
                session->setFailureResult(SyncResults::SYNC_RESULT_FAILED, aErrorCode);
                if (session->isProfileCreated()) {
                    iProfileManager.removeProfile(session->profileName());
                }

                QDateTime nextRetryInterval = iProfileManager.getNextRetryInterval(session->profile());
                if (nextRetryInterval.isValid()) {
                    if (iSyncScheduler) {
                        // can be null if in backup/restore state.
                        iSyncScheduler->addProfileForSyncRetry(session->profile(), nextRetryInterval);
                    }
                } else {
                    iProfileManager.retriesDone(session->profile()->name());
                }
                break;
            }

            default:
                qCWarning(lcButeoMsyncd) << "Unhandled Status in onSessionFinished" << aStatus;
                break;
            }

            iActiveSessions.remove(aProfileName);
            if (session->isScheduled()) {
                // Calling this multiple times has no effect, even if the
                // session was not actually opened
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                qCWarning(lcButeoMsyncd) << "Call to dissconect session not support";
#else
                iNetworkManager->disconnectSession();
#endif
            }
            cleanupSession(session, aStatus);
            if (iProfilesToRemove.contains(aProfileName)) {
                cleanupProfile(aProfileName);
                iProfilesToRemove.removeAll(aProfileName);
            }
            if (session->isAborted() && (iActiveSessions.size() == 0) && isBackupRestoreInProgress()) {
                stopServers();
                iSyncBackup->sendReply(0);
            }
        } else {
            qCWarning(lcButeoMsyncd) << "Session found in active sessions, but is NULL";
        }
    } else {
        qCWarning(lcButeoMsyncd) << "Session not found from active sessions";
    }

    emit syncStatus(aProfileName, aStatus, aMessage, aErrorCode);
    emit syncDone(aProfileName);

    //Re-enable sync on change
    if (iSOCEnabled) {
        iSyncOnChange.enable();
    }

    // Try starting new sync sessions waiting in the queue.
    while (startNextSync()) {
        //intentionally empty
    }
}

void Synchronizer::onSyncProgressDetail(const QString &aProfileName, int aProgressDetail)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    qCDebug(lcButeoMsyncd) << "aProfileName" << aProfileName;
    emit syncStatus(aProfileName, Sync::SYNC_PROGRESS, "Sync Progress", aProgressDetail);
}

bool Synchronizer::startNextSync()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (iSyncQueue.isEmpty() || isBackupRestoreInProgress()) {
        return false;
    }

    bool tryNext = true;

    SyncSession *session = iSyncQueue.head();
    if (session == 0) {
        qCWarning(lcButeoMsyncd) << "Null session found from queue";
        iSyncQueue.dequeue();
        return true;
    }

    SyncProfile *profile = session->profile();
    if (profile == 0) {
        qCWarning(lcButeoMsyncd) << "Null profile found from queued session";
        cleanupSession(session, Sync::SYNC_ERROR);
        iSyncQueue.dequeue();
        return true;
    }

    QString profileName = session->profileName();
    qCDebug(lcButeoMsyncd) << "Trying to start next sync in queue. Profile:" << profileName << session->isScheduled();

    if (session->isScheduled() && iBatteryInfo->isLowPower()) {
        qCWarning(lcButeoMsyncd) << "Low power, scheduled sync aborted";
        iSyncQueue.dequeue();
        session->setFailureResult(SyncResults::SYNC_RESULT_FAILED, Buteo::SyncResults::LOW_BATTERY_POWER);
        cleanupSession(session, Sync::SYNC_ERROR);
        emit syncStatus(profileName, Sync::SYNC_ERROR, "Low Battery", Buteo::SyncResults::LOW_BATTERY_POWER);
        tryNext = true;
    } else if (!session->reserveStorages(&iStorageBooker)) {
        qCDebug(lcButeoMsyncd) << "Needed storage(s) already in use";
        tryNext = false;
    } else if (clientProfileActive(profile->clientProfile()->name())) {
        qCDebug(lcButeoMsyncd) << "Client profile active, wait for finish";
        return false;
    } else {
        // Sync can be started now.
        iSyncQueue.dequeue();
        if (startSyncNow(session)) {
            emit syncStatus(profileName, Sync::SYNC_STARTED, "", 0);
        } else {
            qCWarning(lcButeoMsyncd) << "unable to start sync with session:" << session->profileName();
            session->setFailureResult(SyncResults::SYNC_RESULT_FAILED, Buteo::SyncResults::INTERNAL_ERROR);
            cleanupSession(session, Sync::SYNC_ERROR);
            emit syncStatus(profileName, Sync::SYNC_ERROR, "Internal Error", Buteo::SyncResults::INTERNAL_ERROR);
        }
        tryNext = true;
    }

    return tryNext;
}

void Synchronizer::cleanupSession(SyncSession *aSession, Sync::SyncStatus aStatus)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (aSession != 0) {
        QString profileName = aSession->profileName();
        if (!profileName.isEmpty()) {
            qCDebug(lcButeoMsyncd) << "Clean up session for profile" << profileName;
            SyncProfile *profile = aSession->profile();
            if ((profile->lastResults() == 0) && (aStatus == Sync::SYNC_DONE)) {
                iProfileManager.saveRemoteTargetId(*profile, aSession->results().getTargetId());
            }
            iProfileManager.saveSyncResults(profileName, aSession->results());

            // UI needs to know that Sync Log has been updated.
            emit resultsAvailable(profileName, aSession->results().toString());

            if (aSession->isScheduled()) {
                reschedule(profileName);
                emit signalProfileChanged(profileName, 1, QString());
            }
        }
        aSession->setProfileCreated(false);
        aSession->releaseStorages();
        aSession->deleteLater();
        aSession = 0;
    }
}

void Synchronizer::abortSync(QString aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Abort sync requested for profile: " << aProfileName;
    if (iActiveSessions.contains(aProfileName)) {
        iActiveSessions[aProfileName]->abort();
    } else {
        qCWarning(lcButeoMsyncd) << "No sync in progress with the given profile";
        // Check if sync was queued, in which case, remove it from the queue
        SyncSession *queuedSession = iSyncQueue.dequeue(aProfileName);
        if (queuedSession) {
            qCDebug(lcButeoMsyncd) << "Removed queued sync" << aProfileName;
            delete queuedSession;
        }
        SyncResults syncResults(QDateTime::currentDateTime(), SyncResults::SYNC_RESULT_CANCELLED, Buteo::SyncResults::ABORTED);
        iProfileManager.saveSyncResults(aProfileName, syncResults);
        emit syncStatus(aProfileName, Sync::SYNC_CANCELLED, "", Buteo::SyncResults::ABORTED);
    }
}

bool Synchronizer::cleanupProfile(const QString &aProfileId)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    // We assume this call is made on a Sync Profile
    SyncProfile *profile = iProfileManager.syncProfile (aProfileId);
    bool status = false;

    if (!aProfileId.isEmpty() && profile)  {

        bool client = true ;
        Profile *subProfile = profile->clientProfile(); // client or server
        if (!subProfile) {
            qCWarning(lcButeoMsyncd) << "Could not find client sub-profile";
            subProfile = profile->serverProfile ();
            client = false;
            if (!subProfile) {
                qCWarning(lcButeoMsyncd) << "Could not find server sub-profile";
                return status;
            }
        }

        if (profile->syncType() == SyncProfile::SYNC_SCHEDULED && iSyncScheduler) {
            iSyncScheduler->removeProfile(aProfileId);
        }

        // Remove external sync status if it exist
        removeExternalSyncStatus(profile);

        PluginRunner *pluginRunner;
        if (client) {
            pluginRunner = new ClientPluginRunner(subProfile->name(), profile, &iPluginManager, this, this);
        } else {
            pluginRunner = new ServerPluginRunner(subProfile->name(), profile, &iPluginManager, this,
                                                  iServerActivator, this);
        }

        if (!pluginRunner->init()) {
            qCWarning(lcButeoMsyncd) << "Failed to initialize client plug-in runner";
            delete profile;
            return status;
        }

        const SyncResults *syncResults = profile->lastResults();
        if (!pluginRunner->cleanUp() && syncResults) {
            qCCritical(lcButeoMsyncd) << "Error in removing anchors, sync session ";
        } else {
            qCDebug(lcButeoMsyncd) << "Removing the profile";
            iProfileManager.removeProfile(aProfileId);
            status = true;
        }
        delete profile;
        delete pluginRunner;
    }
    return status;
}

bool Synchronizer::clientProfileActive(const QString &clientProfileName)
{
    QList<SyncSession *> activeSessions = iActiveSessions.values();
    foreach (SyncSession *session, activeSessions) {
        if (session->profile()) {
            SyncProfile *profile = session->profile();
            if (profile->clientProfile()->name() == clientProfileName) {
                return true;
            }
        }
    }
    return false;
}

bool Synchronizer::removeProfile(QString aProfileId)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    bool status = true;

    // Check if a sync session is ongoing for this profile.
    if (iActiveSessions.contains(aProfileId)) {
        // If yes, abort that sync session first
        qCDebug(lcButeoMsyncd) << "Sync still ongoing for profile" << aProfileId;
        qCDebug(lcButeoMsyncd) << "Aborting sync for profile" << aProfileId;
        abortSync(aProfileId);
        iProfilesToRemove.append(aProfileId);
    } else {
        status = cleanupProfile(aProfileId);
    }
    return status;
}

bool Synchronizer::updateProfile(QString aProfileAsXml)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    bool status = false;
    QString address;

    if (!aProfileAsXml.isEmpty())  {
        // save the changes to persistent storage
        Profile *profile = iProfileManager.profileFromXml(aProfileAsXml);
        if (profile) {
            // Update storage info before giving it to profileManager
            /*
            Profile* service = 0;
            foreach(Profile* p, profile->allSubProfiles()) {
            if (p->type() == Profile::TYPE_SERVICE) {
                service = p;
                break;
            }
            }
            */

            if (!profile->boolKey(Buteo::KEY_STORAGE_UPDATED)) {
                address = profile->key(Buteo::KEY_BT_ADDRESS);
                if (!address.isNull()) {
                    if (profile->key(Buteo::KEY_UUID).isEmpty()) {
                        QString uuid = QUuid::createUuid().toString();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                        uuid = uuid.remove(QRegularExpression("[{}]"));
#else
                        uuid = uuid.remove(QRegExp("[{}]"));
#endif
                        profile->setKey(Buteo::KEY_UUID, uuid);
                    }
                    if (profile->key(Buteo::KEY_REMOTE_NAME).isEmpty()) {
                        profile->setKey(Buteo::KEY_REMOTE_NAME, profile->displayname());
                    }
                    profile->setBoolKey(Buteo::KEY_STORAGE_UPDATED, true);
                }
            }

            QString profileId = iProfileManager.updateProfile(*profile);

            // if the profile changes are for schedule sync we need to reschedule
            if (!profileId.isEmpty()) {
                reschedule(profileId);
                status = true;
            }

            delete profile;
        }
    }
    return status;
}

bool Synchronizer::requestStorages(QStringList aStorageNames)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    return iStorageBooker.reserveStorages(aStorageNames, "");
}

void Synchronizer::releaseStorages(QStringList aStorageNames)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    iStorageBooker.releaseStorages(aStorageNames);
    emit storageReleased();
}

QStringList Synchronizer::runningSyncs()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    return iActiveSessions.keys();
}

void Synchronizer::onStorageReleased()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Storage released";
    while (startNextSync()) {
        // Intentionally empty.
    }
}

void Synchronizer::onTransferProgress(const QString &aProfileName,
                                      Sync::TransferDatabase aDatabase, Sync::TransferType aType,
                                      const QString &aMimeType, int aCommittedItems)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Sync session progress";
    qCDebug(lcButeoMsyncd) << "Profile:" << aProfileName;
    qCDebug(lcButeoMsyncd) << "Database:" << aDatabase;
    qCDebug(lcButeoMsyncd) << "Transfer type:" << aType;
    qCDebug(lcButeoMsyncd) << "Mime type:" << aMimeType;

    emit transferProgress(aProfileName, aDatabase, aType, aMimeType, aCommittedItems);

}

void Synchronizer::onStorageAccquired(const QString &aProfileName,
                                      const QString &aMimeType)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    qCDebug(lcButeoMsyncd) << "Mime type:" << aMimeType;
    qCDebug(lcButeoMsyncd) << "Profile:" << aProfileName;
    if (!aProfileName.isEmpty() && !aMimeType.isEmpty()) {
        SyncSession *session = qobject_cast<SyncSession *>(QObject::sender());
        if (session) {
            QMap<QString, bool> storageMap = session->getStorageMap();
            if (aMimeType.compare(QString("text/x-vcard"), Qt::CaseInsensitive) == 0)
                storageMap["hcontacts"] = true;
            else if (aMimeType.compare(QString("text/x-vcalendar"), Qt::CaseInsensitive) == 0)
                storageMap["hcalendar"] = true;
            else if (aMimeType.compare(QString("text/plain"), Qt::CaseInsensitive) == 0)
                storageMap["hnotes"] = true;
#ifdef BM_SYNC
            else if (aMimeType.compare(QString("text/x-vbookmark"), Qt::CaseInsensitive) == 0)
                storageMap["hbookmarks"] = true;
#endif
#ifdef SMS_SYNC
            else if (aMimeType.compare(QString("text/x-vmsg"), Qt::CaseInsensitive) == 0)
                storageMap["hsms"] = true;
#endif
            else
                qCDebug(lcButeoMsyncd) << "Unsupported mime type" << aMimeType;

            session->setStorageMap(storageMap);
        }
    }
}

bool Synchronizer::requestStorage(const QString &aStorageName,
                                  const SyncPluginBase *aCaller)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    return iStorageBooker.reserveStorage(aStorageName,
                                         aCaller->getProfileName());
}

void Synchronizer::releaseStorage(const QString &aStorageName,
                                  const SyncPluginBase */*aCaller*/)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    iStorageBooker.releaseStorage(aStorageName);
    emit storageReleased();
}

StoragePlugin *Synchronizer::createStorage(const QString &aPluginName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    StoragePlugin *plugin = nullptr;
    if (!aPluginName.isEmpty()) {
        plugin = iPluginManager.createStorage(aPluginName);
    }

    return plugin;
}

void Synchronizer::initializeScheduler()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    if (!iSyncScheduler) {
        iSyncScheduler = new SyncScheduler(this);
        connect(iSyncScheduler, SIGNAL(syncNow(QString)),
                this, SLOT(startScheduledSync(QString)), Qt::QueuedConnection);
        connect(iSyncScheduler, SIGNAL(externalSyncChanged(QString, bool)),
                this, SLOT(reportExternalSyncStatus(QString, bool)), Qt::QueuedConnection);
        QList<SyncProfile *> profiles = iProfileManager.allSyncProfiles();
        foreach (SyncProfile *profile, profiles) {
            if (profile->syncType() == SyncProfile::SYNC_SCHEDULED) {
                iSyncScheduler->addProfile(profile);
            }
            // Emit external sync status for all profiles
            // on startup, in case of a crash/abort of this
            // process, we should update pontential listeners with
            // the correct status
            reportExternalSyncStatus(profile, true);
        }
        qDeleteAll(profiles);
    }
}

void Synchronizer::destroyStorage(StoragePlugin *aStorage)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    iPluginManager.destroyStorage(aStorage);
}

bool Synchronizer::isConnectivityAvailable(Sync::ConnectivityType aType)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (iTransportTracker != 0) {
        return iTransportTracker->isConnectivityAvailable(aType);
    } else {
        return false;
    }
}

void Synchronizer::startServers(bool resume)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Starting/Resuming server plug-ins";

    if (iServerActivator != 0) {
        if (false == resume) {
            connect(iServerActivator, SIGNAL(serverEnabled(const QString &)),
                    this, SLOT(startServer(const QString &)), Qt::QueuedConnection);

            connect(iServerActivator, SIGNAL(serverDisabled(const QString &)),
                    this, SLOT(stopServer(const QString &)), Qt::QueuedConnection);
        }

        QStringList enabledServers = iServerActivator->enabledServers();
        foreach (QString server, enabledServers) {
            if (false == resume) {
                startServer(server);
            } else {
                ServerPluginRunner *pluginRunner = iServers[server];
                if (pluginRunner) {
                    pluginRunner->resume();
                }
            }
        }
    } else {
        qCCritical(lcButeoMsyncd) << "No server plug-in activator";
    }
}

void Synchronizer::stopServers(bool suspend)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Stopping/Suspending all server plug-ins";

    if (false == suspend) {
        iServerActivator->disconnect();
    }

    QStringList activeServers = iServers.keys();
    foreach (QString server, activeServers) {
        if (false == suspend) {
            stopServer(server);
        } else {
            ServerPluginRunner *pluginRunner = iServers[server];
            if (pluginRunner) {
                pluginRunner->suspend();
            }
        }
    }
}

void Synchronizer::startServer(const QString &aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Starting server plug-in:" << aProfileName;

    if (iServers.contains(aProfileName)) {
        qCWarning(lcButeoMsyncd) << "Server thread already running for profile:" << aProfileName;
        // Remove reference from the activator
        iServerActivator->removeRef(aProfileName, false);
        return;
    }
    Profile *serverProfile = iProfileManager.profile(
                                 aProfileName, Profile::TYPE_SERVER);

    if (!serverProfile) {
        // @todo: for now, do not enforce server plug-ins to have an XML profile
        qCWarning(lcButeoMsyncd) << "Profile not found, creating an empty one";

        ProfileFactory pf;
        serverProfile = pf.createProfile(aProfileName, Profile::TYPE_SERVER);
    } else {
        iProfileManager.expand(*serverProfile);
    }

    if (!serverProfile || !serverProfile->isValid()) {
        qCWarning(lcButeoMsyncd) << "Profile not found or not valid:"  << aProfileName;
        delete serverProfile;
        serverProfile = 0;
        return;
    }

    ServerPluginRunner *pluginRunner = new ServerPluginRunner(aProfileName,
                                                              serverProfile, &iPluginManager, this, iServerActivator, this);

    // Relay connectivity state change signal to plug-in runner.
    connect(iTransportTracker, SIGNAL(connectivityStateChanged(Sync::ConnectivityType, bool)),
            pluginRunner, SIGNAL(connectivityStateChanged(Sync::ConnectivityType, bool)));


    connect(pluginRunner, SIGNAL(done()), this, SLOT(onServerDone()));

    connect(pluginRunner, SIGNAL(newSession(const QString &)),
            this, SLOT(onNewSession(const QString &)));

    if (!pluginRunner->init() || !pluginRunner->start()) {
        qCCritical(lcButeoMsyncd) << "Failed to start plug-in";
        delete pluginRunner;
        pluginRunner = 0;
        return;
    }

    iServers.insert(aProfileName, pluginRunner);

}

void Synchronizer::stopServer(const QString &aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "Stopping server:" << aProfileName;

    if (iServers.contains(aProfileName)) {
        ServerPluginRunner *pluginRunner = iServers[aProfileName];
        if (pluginRunner) {
            pluginRunner->stop();
        }
        qCDebug(lcButeoMsyncd) << "Deleting server";
        if (!iClosing) {
            // This function may have been invoked from a signal. The plugin runner
            // will only be deleted when the server thread returns.
            qCWarning(lcButeoMsyncd) << "The server thread for profile: " << aProfileName <<
                        "is still running. Server will be deleted later";
        } else {
            iServers.remove(aProfileName);
            // Synchronizer is closing, this function is not invoked by a signal.
            // Delete plug-in runner immediately.
            delete pluginRunner;
        }
        pluginRunner = 0;
    } else {
        qCWarning(lcButeoMsyncd) << "Server not found";
    }
}

void Synchronizer::onServerDone()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    ServerPluginRunner *pluginRunner =
        qobject_cast<ServerPluginRunner *>(QObject::sender());
    QString serverName = "unknown";
    if (pluginRunner != 0) {
        serverName = pluginRunner->pluginName();
    }
    qCDebug(lcButeoMsyncd) << "Server stopped:" << serverName;
    if (iServers.values().contains(pluginRunner)) {
        qCDebug(lcButeoMsyncd) << "Deleting server";
        iServers.remove(iServers.key(pluginRunner));
        pluginRunner->deleteLater();
        pluginRunner = 0;
    }
}

bool syncProfilePointerLessThan(SyncProfile *&aLhs, SyncProfile *&aRhs)
{
    if (aLhs && aRhs) {
        if (aLhs->isHidden() != aRhs->isHidden())
            return !aLhs->isHidden();
        if (aLhs->isEnabled() != aRhs->isEnabled())
            return aLhs->isEnabled();
    }

    return false;
}

void Synchronizer::onNewSession(const QString &aDestination)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    qCDebug(lcButeoMsyncd) << "New session from" << aDestination;
    bool createNewProfile = false;
    ServerPluginRunner *pluginRunner =
        qobject_cast<ServerPluginRunner *>(QObject::sender());
    if (pluginRunner != 0) {
        SyncProfile *profile = 0;
        QList<SyncProfile *> syncProfiles;

        if (aDestination.contains("USB")) {
            syncProfiles = iProfileManager.getSyncProfilesByData(
                               QString(), QString(), KEY_DISPLAY_NAME, PC_SYNC);
        } else {
            syncProfiles = iProfileManager.getSyncProfilesByData(
                               QString(), Profile::TYPE_SYNC, KEY_BT_ADDRESS, aDestination);
        }
        if (syncProfiles.isEmpty()) {
            qCDebug(lcButeoMsyncd) << "No sync profiles found with a matching destination address";
            // destination a bt address
            profile = iProfileManager.createTempSyncProfile(aDestination, createNewProfile);
            profile->setKey(Buteo::KEY_UUID, iUUID);
            profile->setKey(Buteo::KEY_REMOTE_NAME, iRemoteName);
            if (createNewProfile) {
                iProfileManager.updateProfile(*profile);
            }
        } else {
            // Sort profiles to preference order. Visible and enabled are preferred.
            std::sort(syncProfiles.begin(), syncProfiles.end(), syncProfilePointerLessThan);

            profile = syncProfiles.first();
            qCDebug(lcButeoMsyncd) << "Found" << syncProfiles.count() << "sync profiles with a "
                       "matching destination address. Selecting" <<
                       profile->name();
            syncProfiles.removeFirst();
            qDeleteAll(syncProfiles);
        }
        // If the profile is not hidden, UI must be informed.
        if (!profile->isHidden()) {
            // Get the DBUS interface for sync-UI.
            qCDebug(lcButeoMsyncd) << "sync-ui dbus interface is getting called";
            if (iSyncUIInterface == nullptr) {
                qCDebug(lcButeoMsyncd) << "iSyncUIInterface is nullptr";
                iSyncUIInterface = new QDBusInterface("com.nokia.syncui", "/org/maemo/m",
                                                      "com.nokia.MApplicationIf", QDBusConnection::sessionBus());
            } else if (!iSyncUIInterface->isValid()) {
                qCDebug(lcButeoMsyncd) << "iSyncUIInterface is not Valid()";
                delete iSyncUIInterface;
                iSyncUIInterface = nullptr;
                iSyncUIInterface = new QDBusInterface("com.nokia.syncui", "/org/maemo/m",
                                                      "com.nokia.MApplicationIf", QDBusConnection::sessionBus());

            }
            //calling launch with argument list
            QStringList list;
            list.append("launching");
            QList<QVariant> argumentList;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            argumentList << QVariant::fromValue(list);
#else
            argumentList << qVariantFromValue(list);
#endif
            iSyncUIInterface->asyncCallWithArgumentList(QLatin1String("launch"), argumentList);
        }

        SyncSession *session = new SyncSession(profile, this);
        qCDebug(lcButeoMsyncd) << "Disable sync on change";
        //As sync is ongoing, disable sync on change for now, we can query later if
        //there are changes.
        if (iSOCEnabled) {
            iSyncOnChange.disableNext();
        }

        session->setProfileCreated(createNewProfile);
        // disable all storages
        // @todo : Can we remove hardcoding of the storageNames ???
        QMap<QString, bool> storageMap;
        storageMap["hcontacts"] = 0;
        storageMap["hcalendar"] = 0;
        storageMap["hnotes"] = 0;
#ifdef BM_SYNC
        storageMap["hbookmarks"] = 0;
#endif
#ifdef SMS_SYNC
        storageMap["hsms"] = 0;
#endif

        session->setStorageMap(storageMap);

        iActiveSessions.insert(profile->name(), session);

        // Connect signals from sync session.
        connect(session, SIGNAL(transferProgress(const QString &,
                                                 Sync::TransferDatabase, Sync::TransferType, const QString &, int)),
                this, SLOT(onTransferProgress(const QString &,
                                              Sync::TransferDatabase, Sync::TransferType, const QString &, int)));
        connect(session, SIGNAL(storageAccquired(const QString &, const QString &)),
                this, SLOT(onStorageAccquired(const QString &, const QString &)));
        connect(session, SIGNAL(finished(const QString &, Sync::SyncStatus,
                                         const QString &, int)),
                this, SLOT(onSessionFinished(const QString &, Sync::SyncStatus,
                                             const QString &, int)));
        connect(session, SIGNAL(syncProgressDetail(const QString &, int)),
                this, SLOT(onSyncProgressDetail(const QString &, int)));

        // Associate plug-in runner with the new session.
        session->setPluginRunner(pluginRunner, false);
        emit syncStatus(profile->name(), Sync::SYNC_STARTED, "", 0);
    } else {
        qCWarning(lcButeoMsyncd) << "Could not resolve server, session object not created";
    }
}

void Synchronizer::slotProfileChanged(QString aProfileName, int aChangeType, QString aProfileAsXml)
{
    // queue up a sync when a new profile is added or an existing profile is modified.
    // we coalesce changes to profiles so that we do not trigger syncs immediately
    // on change, to avoid thrash during backup/restore and races if the client wishes
    // to trigger manually.  Temporary until we can improve Buteo's SyncOnChange handler.
    switch (aChangeType) {
    case ProfileManager::PROFILE_ADDED: {
        iProfileChangeTriggerQueue.append(qMakePair(aProfileName, ProfileManager::PROFILE_ADDED));
        iProfileChangeTriggerTimer.start(30000); // 30 seconds.
    }
    break;

    case ProfileManager::PROFILE_REMOVED:
        iSyncOnChangeScheduler.removeProfile(aProfileName);
        iWaitingOnlineSyncs.removeAll(aProfileName);
        for (int i = iProfileChangeTriggerQueue.size() - 1; i >= 0; --i) {
            if (iProfileChangeTriggerQueue[i].first == aProfileName) {
                qCDebug(lcButeoMsyncd) << "Removing queued profile change sync due to profile removal:" << aProfileName;
                iProfileChangeTriggerQueue.removeAt(i);
            }
        }
        break;

    case ProfileManager::PROFILE_MODIFIED: {
        bool alreadyQueued = false;
        for (int i = 0; i < iProfileChangeTriggerQueue.size(); ++i) {
            if (iProfileChangeTriggerQueue.at(i).first == aProfileName) {
                alreadyQueued = true;
                break;
            }
        }
        if (!alreadyQueued) {
            iProfileChangeTriggerQueue.append(qMakePair(aProfileName, ProfileManager::PROFILE_MODIFIED));
        }
        iProfileChangeTriggerTimer.start(30000); // 30 seconds.
    }
    break;
    }

    emit signalProfileChanged(aProfileName, aChangeType, aProfileAsXml);
}

void Synchronizer::profileChangeTriggerTimeout()
{
    if (iProfileChangeTriggerQueue.isEmpty()) {
        return;
    }

    QPair<QString, ProfileManager::ProfileChangeType> queuedChange = iProfileChangeTriggerQueue.takeFirst();
    SyncProfile *profile = iProfileManager.syncProfile(queuedChange.first);
    if (profile) {
        if (queuedChange.second == ProfileManager::PROFILE_ADDED) {
            enableSOCSlot(queuedChange.first);
            if (profile->isEnabled()) {
                qCDebug(lcButeoMsyncd) << "Triggering queued profile addition sync for:" << queuedChange.first;
                startSync(queuedChange.first);
            }
        } else if (profile->isEnabled()) {
            qCDebug(lcButeoMsyncd) << "Triggering queued profile modification sync for:" << queuedChange.first;
            startScheduledSync(queuedChange.first);
        }
        delete profile;
    }

    // continue triggering profiles until we have emptied the queue.
    QMetaObject::invokeMethod(this, "profileChangeTriggerTimeout", Qt::QueuedConnection);
}

void Synchronizer::reschedule(const QString &aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (iSyncScheduler == 0)
        return;

    SyncProfile *profile = iProfileManager.syncProfile(aProfileName);

    if (profile && profile->syncType() == SyncProfile::SYNC_SCHEDULED && profile->isEnabled()) {
        iSyncScheduler->addProfile(profile);
    } else {
        qCDebug(lcButeoMsyncd) << "Scheduled sync got disabled for" << aProfileName;
        iSyncScheduler->removeProfile(aProfileName);
    }
    if (profile) {
        reportExternalSyncStatus(profile);
        qCDebug(lcButeoMsyncd) << "Reschdule profile" << aProfileName << profile->syncType() << profile->isEnabled();
        delete profile;
        profile = nullptr;
    }
}

void Synchronizer::slotSyncStatus(QString aProfileName, int aStatus, QString aMessage, int aMoreDetails)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    SyncProfile *profile = iProfileManager.syncProfile(aProfileName);
    if (profile) {
        QString accountId = profile->key(KEY_ACCOUNT_ID);
        if (!accountId.isNull()) {
            switch (aStatus) {
            case Sync::SYNC_QUEUED:
            case Sync::SYNC_STARTED:
            case Sync::SYNC_ERROR:
            case Sync::SYNC_DONE:
            case Sync::SYNC_ABORTED:
            case Sync::SYNC_CANCELLED:
            case Sync::SYNC_NOTPOSSIBLE: {
                qCDebug(lcButeoMsyncd) << "Sync status changed for account" << accountId;
                qlonglong aPrevSyncTime;
                qlonglong aNextSyncTime;
                int aFailedReason;
                int aNewStatus = status(accountId.toUInt(), aFailedReason, aPrevSyncTime, aNextSyncTime);
                emit statusChanged(accountId.toUInt(), aNewStatus, aFailedReason, aPrevSyncTime, aNextSyncTime);
            }
            break;
            case Sync::SYNC_STOPPING:
            case Sync::SYNC_PROGRESS:
            default:
                break;
            }
        }
        if (iSyncScheduler) {
            // can be null if in backup/restore state.
            iSyncScheduler->syncStatusChanged(aProfileName, aStatus, aMessage, aMoreDetails);
        }
        delete profile;
    }
}

void Synchronizer::removeScheduledSync(const QString &aProfileName)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (iSyncScheduler == 0)
        return;

    SyncProfile *profile = iProfileManager.syncProfile(aProfileName);

    if (profile) {
        if (!profile->isEnabled()) {
            qCDebug(lcButeoMsyncd) << "Sync got disabled for" << aProfileName;
            iSyncScheduler->removeProfile(aProfileName);
        }
        // Check if external sync status changed, profile might be turned
        // to sync externally and thus buteo sync set to disable
        reportExternalSyncStatus(profile);
        delete profile;
    }
}

bool Synchronizer::isBackupRestoreInProgress()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    bool retVal = getBackUpRestoreState();

    if (retVal) {
        qCDebug(lcButeoMsyncd) << "Backup-Restore o/p in progress - Failed to start manual sync";
    }

    return retVal;

}

void Synchronizer::backupRestoreStarts()
{
    qCDebug(lcButeoMsyncd) << "Synchronizer:backupRestoreStarts:";

    iClosing = true;
    // No active sessions currently !!
    if (iActiveSessions.size() == 0) {
        qCDebug(lcButeoMsyncd) << "No active sync sessions ";
        stopServers(true);
        iSyncBackup->sendReply(0);
    } else {
        // Stop running sessions
        QList<SyncSession *> sessions = iActiveSessions.values();
        foreach (SyncSession *session, sessions) {
            if (session) {
                session->abort();
            }
        }
    }

    delete iSyncScheduler;
    iSyncScheduler = 0;

    // Request all external syncs to stop, relying on reportExternalSyncStatus() to
    // act appropriately because (getBackUpRestoreState() == true)
    QMap<QString, bool>::iterator syncStatus;
    for (syncStatus = iExternalSyncProfileStatus.begin();
            syncStatus != iExternalSyncProfileStatus.end(); ++syncStatus) {
        const QString &profileName = syncStatus.key();
        reportExternalSyncStatus(profileName, false);
    }
}

void Synchronizer::backupRestoreFinished()
{
    qCDebug(lcButeoMsyncd) << "Synchronizer::backupFinished";
    iClosing = false;
    startServers(true);
    initializeScheduler();
    iSyncBackup->sendReply(0);
}

void Synchronizer::backupStarts()
{
    qCDebug(lcButeoMsyncd) << "Synchronizer::backupStarts";
    emit backupInProgress();
    backupRestoreStarts ();
}

void Synchronizer::backupFinished()
{
    qCDebug(lcButeoMsyncd) << "Synchronizer::backupFinished";
    backupRestoreFinished();
    emit backupDone();
}

void Synchronizer::restoreStarts()
{
    qCDebug(lcButeoMsyncd) << "Synchronizer::restoreStarts";
    emit restoreInProgress();
    backupRestoreStarts();
}

void Synchronizer::restoreFinished()
{
    qCDebug(lcButeoMsyncd) << "Synchronizer::restoreFinished";
    backupRestoreFinished();
    emit restoreDone();
}

bool Synchronizer::getBackUpRestoreState()
{
    qCDebug(lcButeoMsyncd) << "Synchronizer::getBackUpRestoreState";
    return iSyncBackup->getBackUpRestoreState();
}

void Synchronizer::start(unsigned int aAccountId)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    qCDebug(lcButeoMsyncd) << "Start sync requested for account" << aAccountId;
    QList<SyncProfile *> profileList = iAccounts->getProfilesByAccountId(aAccountId);
    foreach (SyncProfile *profile, profileList) {
        startSync(profile->name());
        delete profile;
    }
}

void Synchronizer::stop(unsigned int aAccountId)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    qCDebug(lcButeoMsyncd) << "Stop sync requested for account" << aAccountId;
    QList<SyncProfile *> profileList = iAccounts->getProfilesByAccountId(aAccountId);
    foreach (SyncProfile *profile, profileList) {
        abortSync(profile->name());
        delete profile;
    }
}

int Synchronizer::status(unsigned int aAccountId, int &aFailedReason, qlonglong &aPrevSyncTime,
                         qlonglong &aNextSyncTime)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    int status = 1; // Initialize to Done
    QDateTime prevSyncTime; // Initialize to invalid
    QDateTime nextSyncTime;
    QList<SyncProfile *> profileList = iAccounts->getProfilesByAccountId(aAccountId);
    foreach (SyncProfile *profile, profileList) {
        // First check if sync is going on for any profile corresponding to this
        // account ID
        if (iActiveSessions.contains(profile->name()) || iSyncQueue.contains(profile->name())) {
            qCDebug(lcButeoMsyncd) << "Sync running for" << aAccountId;
            status = 0;
            break;
        } else {
            // Check if the last sync resulted in an error for any of the
            // profiles
            const SyncResults *lastResults = profile->lastResults();
            if (lastResults && SyncResults::SYNC_RESULT_FAILED == lastResults->majorCode()) {
                status = 2;
                // TODO: Determine the set of failure enums needed here
                aFailedReason = lastResults->minorCode();
                break;
            }
        }
    }

    if (status != 0) {
        // Need to return the next and last sync times
        foreach (SyncProfile *profile, profileList) {
            if (!prevSyncTime.isValid()) {
                prevSyncTime = profile->lastSyncTime();
            } else {
                (prevSyncTime > profile->lastSyncTime()) ? prevSyncTime : profile->lastSyncTime();
            }
        }
        if (prevSyncTime.isValid()) {
            // Doesn't really matter which profile we do this for, as all of
            // them have the same schedule
            SyncProfile *profile = profileList.first();
            nextSyncTime = profile->nextSyncTime(prevSyncTime);
        }
    }
    aPrevSyncTime = prevSyncTime.toMSecsSinceEpoch();
    aNextSyncTime = nextSyncTime.toMSecsSinceEpoch();
    qDeleteAll(profileList);
    return status;
}

QList<unsigned int> Synchronizer::syncingAccounts()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    QList<unsigned int> syncingAccountsList;
    // Check active sessions
    QList<SyncSession *> activeSessions = iActiveSessions.values();
    foreach (SyncSession *session, activeSessions) {
        if (session->profile()) {
            SyncProfile *profile = session->profile();
            QString accountId = profile->key(KEY_ACCOUNT_ID);
            if (!accountId.isNull()) {
                syncingAccountsList.append(accountId.toUInt());
            }
        }
    }
    // Check queued syncs
    const QList<SyncSession *> queuedSessions = iSyncQueue.getQueuedSyncSessions();
    foreach (const SyncSession *session, queuedSessions) {
        if (session->profile()) {
            SyncProfile *profile = session->profile();
            QString accountId = profile->key(KEY_ACCOUNT_ID);
            if (!accountId.isNull()) {
                syncingAccountsList.append(accountId.toUInt());
            }
        }
    }
    return syncingAccountsList;
}

QString Synchronizer::getLastSyncResult(const QString &aProfileId)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    QString lastSyncResult;

    if (!aProfileId.isEmpty()) {
        SyncProfile *profile = iProfileManager.syncProfile (aProfileId);
        if (profile) {
            const SyncResults *syncResults = profile->lastResults();
            if (syncResults) {
                lastSyncResult = syncResults->toString();
                qCDebug(lcButeoMsyncd) << "SyncResults found:" << lastSyncResult;
            } else {
                qCDebug(lcButeoMsyncd) << "SyncResults not Found!!!";
            }
            delete profile;
        } else {

            qCDebug(lcButeoMsyncd) << "No profile found with aProfileId" << aProfileId;
        }
    }
    return lastSyncResult;
}

QStringList Synchronizer::allVisibleSyncProfiles()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    QStringList profilesAsXml;

    QList<Buteo::SyncProfile *> profiles = iProfileManager.allVisibleSyncProfiles();

    if (!profiles.isEmpty()) {
        foreach (Buteo::SyncProfile *profile, profiles) {
            if (profile) {
                profilesAsXml.append(profile->toString());
                delete profile;
                profile = nullptr;
            }
        }
    }
    qCDebug(lcButeoMsyncd) << "allVisibleSyncProfiles profilesAsXml" << profilesAsXml;
    return profilesAsXml;
}

QString Synchronizer::syncProfile(const QString &aProfileId)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    QString profileAsXml;

    if (!aProfileId.isEmpty()) {
        SyncProfile *profile = iProfileManager.syncProfile (aProfileId);
        if (profile) {
            profileAsXml.append(profile->toString());
            delete profile;
            profile = nullptr;
        } else {

            qCDebug(lcButeoMsyncd) << "No profile found with aProfileId" << aProfileId;
        }
    }
    qCDebug(lcButeoMsyncd) << "syncProfile profileAsXml" << profileAsXml << "aProfileId" << aProfileId;
    return profileAsXml;
}

QStringList Synchronizer::syncProfilesByKey(const QString &aKey, const QString &aValue)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    qCDebug(lcButeoMsyncd) << "syncProfile key : " << aKey << "Value :" << aValue;
    QStringList profilesAsXml;

    if (!aKey.isEmpty() && !aValue.isEmpty()) {
        QList<ProfileManager::SearchCriteria> filters;
        ProfileManager::SearchCriteria filter;
        filter.iType = ProfileManager::SearchCriteria::EQUAL;
        filter.iKey = aKey;
        filter.iValue = aValue;
        filters.append(filter);
        QList<SyncProfile *> profiles = iProfileManager.getSyncProfilesByData(filters);

        if (profiles.size() > 0) {
            qCDebug(lcButeoMsyncd) << "Found matching profiles  :" << profiles.size();
            foreach (SyncProfile *profile, profiles) {
                profilesAsXml.append(profile->toString());
            }
            qDeleteAll(profiles);
        } else {
            qCDebug(lcButeoMsyncd) << "No profile found with key :" << aKey << "Value : " << aValue;
        }
    }

    return profilesAsXml;
}

QStringList Synchronizer::syncProfilesByType(const QString &aType)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    qCDebug(lcButeoMsyncd) << "Profile Type : " << aType;
    return iProfileManager.profileNames(aType);
}

QStringList Synchronizer::profilesByType(const QString &aType)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    QStringList profilesAsXml;

    qCDebug(lcButeoMsyncd) << "Profile Type : " << aType;
    for (const QString &profileId : iProfileManager.profileNames(aType)) {
        SyncProfile *profile = iProfileManager.syncProfile(profileId);
        if (profile) {
            profilesAsXml.append(profile->toString());
        }
        delete profile;
    }
    qCDebug(lcButeoMsyncd) << "profilesByType profilesAsXml" << profilesAsXml;
    return profilesAsXml;
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void Synchronizer::onNetworkStateChanged(bool aState, QNetworkInformation::TransportMedium type)
#else
void Synchronizer::onNetworkStateChanged(bool aState, Sync::InternetConnectionType type)
#endif
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    qCDebug(lcButeoMsyncd) << "Network state changed: OnLine:" << aState << " connection type:" <<  type;

    if (aState) {
        qCDebug(lcButeoMsyncd) << "Restart sync for profiles that need network, checking profiles:" << iWaitingOnlineSyncs;
        QStringList profiles(iWaitingOnlineSyncs);
        foreach (QString profileName, profiles) {
            SyncProfile *profile = iProfileManager.syncProfile(profileName);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            if (acceptScheduledSync(profile)) {
#else
            if (acceptScheduledSync(aState, type, profile)) {
#endif
                // start sync now, we do not need to call 'startScheduledSync' since that function
                // only checks for internet connection
                iWaitingOnlineSyncs.removeOne(profileName);
                startSync(profileName, true);
            }
            delete profile;
        }
    } else if (!aState) {
        QList<QString> profiles = iActiveSessions.keys();
        foreach (QString profileId, profiles) {
            //Getting profile
            SyncProfile *profile = iProfileManager.syncProfile (profileId);
            if (profile) {
                if (profile->destinationType() == Buteo::SyncProfile::DESTINATION_TYPE_ONLINE) {
                    iActiveSessions[profileId]->abort(Sync::SYNC_ERROR);
                }
                delete profile;
            } else {
                qCDebug(lcButeoMsyncd) << "No profile found with aProfileId" << profileId;
            }
        }
    }
}

Profile *Synchronizer::getSyncProfileByRemoteAddress(const QString &aAddress)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    Profile *profile = 0;
    QList<SyncProfile *> profiles;
    if ("USB" == aAddress) {
        profiles = iProfileManager.getSyncProfilesByData(
            QString(), QString(), KEY_DISPLAY_NAME, PC_SYNC);
    } else {
        profiles = iProfileManager.getSyncProfilesByData("",
                                                         Buteo::Profile::TYPE_SYNC,
                                                         Buteo::KEY_BT_ADDRESS,
                                                         aAddress);
    }
    if (!profiles.isEmpty()) {
        profile = profiles.first();
    }
    return profile;
}

QString Synchronizer::getValue(const QString &aAddress, const QString &aKey)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    QString value;
    if (Buteo::KEY_UUID == aKey) {
        iUUID = QUuid::createUuid().toString();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        iUUID = iUUID.remove(QRegularExpression("[{}]"));
#else
        iUUID = iUUID.remove(QRegExp("[{}]"));
#endif
        value = iUUID;
    }

    if (Buteo::KEY_REMOTE_NAME == aKey) {
        if ("USB" == aAddress) {
            iRemoteName = PC_SYNC;
        } else {
            BtHelper btHelper(aAddress);
            iRemoteName = btHelper.getDeviceProperties().value(BT_PROPERTIES_NAME).toString();
        }
        value = iRemoteName;
    }
    return value;
}

void Synchronizer::reportExternalSyncStatus(const QString &aProfileName, bool force)
{
    SyncProfile *profile = iProfileManager.syncProfile(aProfileName);
    if (profile) {
        reportExternalSyncStatus(profile, force);
        delete profile;
    }
}

// Here we store profile names since they are unique, but can be anything, and we emit signals
// containing the client profile name, since those are always associated with a
// specific plugin, this way potential listeners of these signals can distinguish the signals
// based on the accountId and client profile name.
void Synchronizer::reportExternalSyncStatus(const SyncProfile *aProfile, bool force)
{
    int accountId = aProfile->key(KEY_ACCOUNT_ID).toInt();
    if (accountId) {
        const QString &profileName = aProfile->name();
        const QString &clientProfile = aProfile->clientProfile()->name();

        // All external syncs are stopped while a backup or restore is running
        if (getBackUpRestoreState()) {
            if (iExternalSyncProfileStatus.value(profileName) || force) {
                qCDebug(lcButeoMsyncd) << "Sync externally status suspended during backup for profile:" << profileName;
                iExternalSyncProfileStatus.insert(profileName, false);
                emit syncedExternallyStatus(accountId, clientProfile, false);
            }
            // Account in set to sync externally, buteo will let external process handle the syncs in this case
        } else if (aProfile->syncExternallyEnabled()) {
            if (!iExternalSyncProfileStatus.value(profileName)) {
                qCDebug(lcButeoMsyncd) << "Sync externally status changed from false to true for profile:" << profileName;
                iExternalSyncProfileStatus.insert(profileName, true);
                emit syncedExternallyStatus(accountId, clientProfile, true);
            } else if (force) {
                qCDebug(lcButeoMsyncd) << "Account is in set to sync externally for profile:" << profileName;
                emit syncedExternallyStatus(accountId, clientProfile, true);
            }
            // Account set to sync externally in rush mode
        } else if (aProfile->syncExternallyDuringRush()) {
            // Check if we are currently inside rush
            bool isSyncExternally = aProfile->inExternalSyncRushPeriod();
            if (iExternalSyncProfileStatus.contains(profileName)) {
                qCDebug(lcButeoMsyncd) << "We already have this profile, lets check the status for profile:" << profileName;
                bool prevSyncExtState = iExternalSyncProfileStatus.value(profileName);
                if (prevSyncExtState != isSyncExternally) {
                    iExternalSyncProfileStatus.insert(profileName, isSyncExternally);
                    qCDebug(lcButeoMsyncd) << "Sync externally status changed to " << isSyncExternally << "for profile:" << profileName;
                    emit syncedExternallyStatus(accountId, clientProfile, isSyncExternally);
                } else if (force) {
                    qCDebug(lcButeoMsyncd) << "Sync externally status did not change, current state is: " << prevSyncExtState << "for profile:" <<
                              profileName;
                    emit syncedExternallyStatus(accountId, clientProfile, prevSyncExtState);
                }
            } else {
                iExternalSyncProfileStatus.insert(profileName, isSyncExternally);
                qCDebug(lcButeoMsyncd) << "Inserting sync externally status:" << isSyncExternally << "for profile:" << profileName;
                emit syncedExternallyStatus(accountId, clientProfile, isSyncExternally);
            }
        } else {
            if (iExternalSyncProfileStatus.contains(profileName)) {
                iExternalSyncProfileStatus.remove(profileName);
                emit syncedExternallyStatus(accountId, clientProfile, false);
                qCDebug(lcButeoMsyncd) << "Removing sync externally status for profile:" << profileName;
            } else if (force) {
                qCDebug(lcButeoMsyncd) << "Sync externally is off for profile:" << profileName;
                emit syncedExternallyStatus(accountId, clientProfile, false);
            }
        }
    }
}

void Synchronizer::removeExternalSyncStatus(const SyncProfile *aProfile)
{
    int accountId = aProfile->key(KEY_ACCOUNT_ID).toInt();
    if (accountId) {
        const QString &profileName = aProfile->name();
        if (iExternalSyncProfileStatus.contains(profileName)) {
            // if profile was set to sync externally emit the change state signal
            if (iExternalSyncProfileStatus.value(profileName)) {
                emit syncedExternallyStatus(accountId, aProfile->clientProfile()->name(), false);
            }
            iExternalSyncProfileStatus.remove(profileName);
            qCDebug(lcButeoMsyncd) << "Removing sync externally status for profile:" << profileName;
        }
    }
}
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool Synchronizer::acceptScheduledSync(SyncProfile *aSyncProfile) const
#else
bool Synchronizer::acceptScheduledSync(bool aConnected, Sync::InternetConnectionType aType,
                                       SyncProfile *aSyncProfile) const
#endif
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    if (iNetworkManager->reachability() != QNetworkInformation::Reachability::Online) {
#else
    if (!aConnected) {
#endif
        qCWarning(lcButeoMsyncd) << "Scheduled sync refused, not connected";
        return false;
    }

    if (!aSyncProfile) {
        qCWarning(lcButeoMsyncd) << "Scheduled sync refused, invalid sync profile";
        return false;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QList<QNetworkInformation::TransportMedium> allowedTypes = aSyncProfile->internetConnectionTypes();
    if(iNetworkManager->transportMedium() != QNetworkInformation::TransportMedium::Unknown && !allowedTypes.isEmpty()) {
        return allowedTypes.contains(iNetworkManager->transportMedium());
    }

    if(iNetworkManager->transportMedium() == QNetworkInformation::TransportMedium::Ethernet
        || iNetworkManager->transportMedium() == QNetworkInformation::TransportMedium::WiFi) {
        return true;
    }

    if (g_settings_get_boolean(iSettings, "allow-scheduled-sync-over-cellular")) {
        qCInfo(lcButeoMsyncd) << "Allowing sync for cellular/other connection type:" << iNetworkManager->transportMedium();
        return true;
    }
    qCWarning(lcButeoMsyncd) << "Scheduled sync refused, profile disallows current connection type:" << iNetworkManager->transportMedium();
#else
    QList<Sync::InternetConnectionType> allowedTypes = aSyncProfile->internetConnectionTypes();
    if (aType != Sync::INTERNET_CONNECTION_UNKNOWN && !allowedTypes.isEmpty()) {
        return allowedTypes.contains(aType);
    }

    // If no allowed types are specified, fallback to the old default settings.
    if (aType == Sync::INTERNET_CONNECTION_WLAN
            || aType == Sync::INTERNET_CONNECTION_ETHERNET) {
        return true;
    }
    if (g_settings_get_boolean(iSettings, "allow-scheduled-sync-over-cellular")) {
        qCInfo(lcButeoMsyncd) << "Allowing sync for cellular/other connection type:" << aType;
        return true;
    }
    qCWarning(lcButeoMsyncd) << "Scheduled sync refused, profile disallows current connection type:" << aType;
#endif
    return false;
}

void Synchronizer::isSyncedExternally(unsigned int aAccountId, const QString aClientProfileName)
{
    qCDebug(lcButeoMsyncd) << "Received isSyncedExternally request for account:" << aAccountId;
    bool profileFound = false;
    QList<SyncProfile *> syncProfiles = iAccounts->getProfilesByAccountId(aAccountId);
    if (!syncProfiles.isEmpty()) {
        foreach (SyncProfile *profile, syncProfiles) {
            if (profile->clientProfile()->name() == aClientProfileName) {
                reportExternalSyncStatus(profile, true);
                profileFound = true;
                break;
            }
        }
    }
    if (!profileFound) {
        qCDebug(lcButeoMsyncd) << "We don't have a profile for account:" << aAccountId << "emitting sync external status false";
        emit syncedExternallyStatus(aAccountId, QString(), false);
    }
    qDeleteAll(syncProfiles);
}
