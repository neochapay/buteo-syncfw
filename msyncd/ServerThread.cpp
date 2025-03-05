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
#include "ServerThread.h"
#include "ServerPlugin.h"
#include "LogMacros.h"
#include <QMutexLocker>
#include <QCoreApplication>

using namespace Buteo;


ServerThread::ServerThread()
    : iServerPlugin(0)
    , iRunning(false)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
}

ServerThread::~ServerThread()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
}

QString ServerThread::getProfileName() const
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    QString profileName;
    if (iServerPlugin != 0) {
        profileName = iServerPlugin->getProfileName();
    }

    return profileName;
}

ServerPlugin *ServerThread::getPlugin() const
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    return iServerPlugin;
}


bool ServerThread::startThread(ServerPlugin *aServerPlugin)
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (aServerPlugin == 0)
        return false;

    {
        QMutexLocker locker(&iMutex);
        if (iRunning) {
            return false;
        } else {
            iRunning = true;
        }
    }

    iServerPlugin = aServerPlugin;

    // Move to server thread
    iServerPlugin->moveToThread(this);

    start();

    return true;
}

void ServerThread::stopThread()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);
    exit();
}

void ServerThread::run()
{
    FUNCTION_CALL_TRACE(lcButeoTrace);

    if (iServerPlugin == 0) {
        qCCritical(lcButeoMsyncd) << "Server plug-in is NULL";
        return;
    }

    if (!iServerPlugin->init()) {
        qCWarning(lcButeoMsyncd) << "Could not initialize server plugin:" << iServerPlugin->getPluginName();
        emit initError(iServerPlugin->getProfileName(), "", SyncResults::PLUGIN_ERROR);
        return;
    }

    if (!iServerPlugin->startListen()) {
        qCWarning(lcButeoMsyncd) << "Could not start server plugin:" << iServerPlugin->getPluginName();
        emit initError(iServerPlugin->getProfileName(), "", SyncResults::PLUGIN_ERROR);
        return;
    }

    exec();

    iServerPlugin->stopListen();

    iServerPlugin->uninit();

    // Move back to application thread
    iServerPlugin->moveToThread(QCoreApplication::instance()->thread());

    {
        QMutexLocker locker(&iMutex);
        iRunning = false;
    }
}
