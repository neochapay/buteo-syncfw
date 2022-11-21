/*
 * This file is part of buteo-syncfw package
 *
 * Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
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

#include <QCoreApplication>
#include <QStandardPaths>
#include <QtDebug>
#include <QDateTime>

#include <sys/types.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>

#include "Logger.h"
#include "synchronizer.h"
#include "SyncSigHandler.h"
#include "SyncCommonDefs.h"

Q_DECL_EXPORT int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    Buteo::Synchronizer *synchronizer = new Buteo::Synchronizer(&app);

    if (!synchronizer->initialize()) {
        delete synchronizer;
        synchronizer = 0;
        return -1;
    }

    QString genericCache = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
    QFile::Permissions permissions(QFile::ExeOwner | QFile::ExeGroup | QFile::ReadOwner | QFile::WriteOwner |
                                   QFile::ReadGroup | QFile::WriteGroup);

    // Make sure we have the .cache directory
    QDir homeDir(genericCache);
    if (homeDir.mkpath(genericCache)) {
        uid_t uid = getuid();
        struct passwd *pwd;
        struct group *grp;
        // assumes that correct groupname is same as username (e.g. nemo:nemo)
        pwd = getpwuid(uid);
        if (pwd != nullptr) {
            grp = getgrnam(pwd->pw_name);
            if (grp != nullptr) {
                gid_t gid = grp->gr_gid;
                int ret = chown(genericCache.toLatin1().data(), uid, gid);
                Q_UNUSED(ret);
            }
        }
        QFile::setPermissions(genericCache, permissions);
    }

    QString msyncCacheSyncDir = Sync::syncCacheDir() + QDir::separator() + "sync";

    // Make sure we have the msyncd/sync directory
    QDir syncDir(msyncCacheSyncDir);
    if (syncDir.mkpath(msyncCacheSyncDir)) {
        QFile::setPermissions(Sync::syncCacheDir(), permissions);
        QFile::setPermissions(msyncCacheSyncDir, permissions);
    }

    Buteo::configureLegacyLogging();

    // Note:- Since we can't call Qt functions from Unix signal handlers.
    // This class provide handling unix signal.
    SyncSigHandler *sigHandler = new SyncSigHandler();

    qCDebug(lcButeoMsyncd) << "Entering event loop";
    int returnValue = app.exec();
    qCDebug(lcButeoMsyncd) << "Exiting event loop";

    synchronizer->close();
    delete synchronizer;
    synchronizer = 0;

    delete sigHandler;
    sigHandler = 0;

    qDebug() << "Exiting program";

    return returnValue;
}
