/*
* This file is part of buteo-sync-plugins package
*
* Copyright (C) 2013 - 2021 Jolla Ltd.
*
* Author: Sateesh Kavuri <sateesh.kavuri@gmail.com>
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
*/
#include <QCoreApplication>
#include <QDBusConnection>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QRegularExpression>
#else
#include <QRegExp>
#endif
#include "PluginServiceObj.h"
#include "ButeoPluginIfaceAdaptor.h"
#include "Logger.h"

#define DBUS_SERVICE_NAME_PREFIX "com.buteo.msyncd.plugin."
#define DBUS_SERVICE_OBJ_PATH "/"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    Buteo::configureLegacyLogging();

    // We obtain the plugin name and the profile name from cmdline
    // One way to pass the arguments is via cmdline, the other way is
    // to use the method setPluginParams() dbus method. But setting
    // cmdline arguments is probably cleaner
    QStringList args = app.arguments();

    if (args.length() < 4) {
        qCCritical(lcButeoPlugin) << "Plugin name, profile name and plugin path not obtained from cmdline" ;
    }

    const QString pluginName = args.value(1);
    const QString profileName = args.value(2);
    const QString pluginFilePath = args.value(3);

    PluginServiceObj *serviceObj = new PluginServiceObj(pluginName, profileName, pluginFilePath);

    new ButeoPluginIfaceAdaptor(serviceObj);

    // randomly-generated profile names cannot be registered
    // as dbus service paths due to being purely numeric.
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    int numericIdx = profileName.indexOf(QRegularExpression("[0123456789]"));
#else
    int numericIdx = profileName.indexOf(QRegExp("[0123456789]"));
#endif
    QString servicePath = numericIdx == 0
                          ? QString(QLatin1String("%1%2%3"))
                          .arg(DBUS_SERVICE_NAME_PREFIX)
                          .arg("profile-")
                          .arg(profileName)
                          : QString(QLatin1String("%1%2"))
                          .arg(DBUS_SERVICE_NAME_PREFIX)
                          .arg(profileName);

    int retn;
    qCDebug(lcButeoPlugin) << "attempting to register dbus service:" << servicePath ;
    QDBusConnection connection = QDBusConnection::sessionBus();
    if (connection.registerObject(DBUS_SERVICE_OBJ_PATH, serviceObj)) {
        if (connection.registerService(servicePath)) {
            qCDebug(lcButeoPlugin) << "Plugin " << pluginName << " with profile "
                      << profileName << " registered at dbus "
                      << DBUS_SERVICE_NAME_PREFIX + profileName
                      << " and path " << DBUS_SERVICE_OBJ_PATH;
            // TODO: Should any unix signals be handled?
            retn = app.exec();
            connection.unregisterService(servicePath);
        } else {
            qCWarning(lcButeoPlugin) << "Unable to register dbus service"
                        << servicePath << ", terminating.";
            retn = -1;
        }
        connection.unregisterObject(DBUS_SERVICE_OBJ_PATH);
    } else {
        qCWarning(lcButeoPlugin) << "Unable to register dbus object" << DBUS_SERVICE_OBJ_PATH << "for service"
                    << servicePath << ", terminating.";
        retn = -2;
    }

    delete serviceObj;
    return retn;
}
