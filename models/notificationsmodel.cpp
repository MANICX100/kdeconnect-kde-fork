/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "notificationsmodel.h"

#include <QDebug>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QTimer>

#include <algorithm>
#include <utility>

#include <dbushelper.h>

#include "models_debug.h"

NotificationsModel::NotificationsModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_dbusInterface(nullptr)
{
    connect(this, &QAbstractItemModel::rowsInserted, this, &NotificationsModel::rowsChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &NotificationsModel::rowsChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &NotificationsModel::rowsChanged);

    connect(this, &QAbstractItemModel::dataChanged, this, &NotificationsModel::anyDismissableChanged);
    connect(this, &QAbstractItemModel::rowsInserted, this, &NotificationsModel::anyDismissableChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &NotificationsModel::anyDismissableChanged);

    m_refreshTimer.setInterval(500);
    m_refreshTimer.setSingleShot(true);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] {
        resetDbusInterface();
        refreshNotificationList();
    });

    QDBusServiceWatcher *watcher =
        new QDBusServiceWatcher(DaemonDbusInterface::activatedService(), QDBusConnection::sessionBus(), QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, &m_refreshTimer, qOverload<>(&QTimer::start));
    connect(watcher, &QDBusServiceWatcher::serviceUnregistered, this, [this] {
        m_refreshTimer.stop();
        clearNotifications();
    });
}

QHash<int, QByteArray> NotificationsModel::roleNames() const
{
    // Role names for QML
    QHash<int, QByteArray> names = QAbstractItemModel::roleNames();
    names.insert(DbusInterfaceRole, "dbusInterface");
    names.insert(AppNameModelRole, "appName");
    names.insert(IdModelRole, "notificationId");
    names.insert(DismissableModelRole, "dismissable");
    names.insert(RepliableModelRole, "repliable");
    names.insert(IconPathModelRole, "appIcon");
    names.insert(TitleModelRole, "title");
    names.insert(TextModelRole, "notitext");
    names.insert(ActionsModelRole, "actions");
    names.insert(ActiveModelRole, "active");
    return names;
}

NotificationsModel::~NotificationsModel()
{
}

QString NotificationsModel::deviceId() const
{
    return m_deviceId;
}

void NotificationsModel::setDeviceId(const QString &deviceId)
{
    m_refreshTimer.stop();
    clearNotifications();
    m_deviceId = deviceId;

    resetDbusInterface();
    refreshNotificationList();

    Q_EMIT deviceIdChanged(deviceId);
}

void NotificationsModel::refreshNotificationList()
{
    if (!m_dbusInterface || !m_dbusInterface->isValid()) {
        if (!m_deviceId.isEmpty()) {
            m_refreshTimer.start();
        }
        return;
    }

    const QVariant historyProperty = m_dbusInterface->property("notificationHistory");
    if (!historyProperty.isValid()) {
        m_refreshTimer.start();
        return;
    }

    m_refreshTimer.stop();
    QList<NotificationEntry> notificationList;
    const QByteArray history = historyProperty.toString().toUtf8().trimmed();
    if (!history.isEmpty()) {
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(history, &error);
        if (error.error != QJsonParseError::NoError || !document.isArray()) {
            qCWarning(KDECONNECT_MODELS) << "Invalid notification history JSON:" << error.errorString();
        } else {
            const QJsonArray notifications = document.array();
            notificationList.reserve(notifications.size());
            for (const QJsonValue &value : notifications) {
                if (!value.isObject()) {
                    continue;
                }

                const QJsonObject notification = value.toObject();
                NotificationEntry entry;
                entry.internalId = notification.value(QStringLiteral("internalId")).toString();
                entry.publicId = notification.value(QStringLiteral("publicId")).toString();
                entry.appName = notification.value(QStringLiteral("appName")).toString();
                entry.ticker = notification.value(QStringLiteral("ticker")).toString();
                entry.title = notification.value(QStringLiteral("title")).toString();
                entry.text = notification.value(QStringLiteral("text")).toString();
                entry.dismissable = notification.value(QStringLiteral("dismissable")).toBool();
                entry.repliable = notification.value(QStringLiteral("repliable")).toBool();
                entry.active = notification.value(QStringLiteral("active")).toBool() && !entry.publicId.isEmpty();
                entry.timestamp = notification.value(QStringLiteral("timestamp")).toVariant().toLongLong();

                const QJsonArray actions = notification.value(QStringLiteral("actions")).toArray();
                entry.actions.reserve(actions.size());
                for (const QJsonValue &action : actions) {
                    if (action.isString()) {
                        entry.actions.append(action.toString());
                    }
                }
                notificationList.append(entry);
            }

            std::stable_sort(notificationList.begin(), notificationList.end(), [](const NotificationEntry &left, const NotificationEntry &right) {
                return left.timestamp > right.timestamp;
            });
        }
    }

    beginResetModel();
    for (const NotificationEntry &entry : std::as_const(m_notificationList)) {
        delete entry.dbusInterface;
    }
    m_notificationList = std::move(notificationList);
    for (NotificationEntry &entry : m_notificationList) {
        if (entry.active) {
            entry.dbusInterface = new NotificationDbusInterface(m_deviceId, entry.publicId, this);
            connect(entry.dbusInterface, &NotificationDbusInterface::ready, this, &NotificationsModel::notificationUpdated);
        }
    }
    endResetModel();
}

void NotificationsModel::resetDbusInterface()
{
    delete m_dbusInterface;
    m_dbusInterface = nullptr;

    if (m_deviceId.isEmpty()) {
        return;
    }

    m_dbusInterface = new DeviceNotificationsDbusInterface(m_deviceId, this);
    connect(m_dbusInterface, &DeviceNotificationsDbusInterface::notificationHistoryChanged, this, &NotificationsModel::refreshNotificationList);
}

QVariant NotificationsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_notificationList.count()) {
        return QVariant();
    }

    const NotificationEntry &notification = m_notificationList[index.row()];

    switch (role) {
    case IconModelRole:
        return QIcon::fromTheme(QStringLiteral("device-notifier"));
    case IdModelRole:
        return notification.internalId;
    case NameModelRole:
        return notification.ticker;
    case ContentModelRole:
        return QString(); // To implement in the Android side
    case AppNameModelRole:
        return notification.appName;
    case DbusInterfaceRole:
        return QVariant::fromValue<QObject *>(notification.dbusInterface);
    case DismissableModelRole:
        return notification.dismissable;
    case RepliableModelRole:
        return notification.repliable;
    case IconPathModelRole:
        return notification.dbusInterface && notification.dbusInterface->isValid() ? notification.dbusInterface->iconPath() : QString();
    case TitleModelRole:
        return notification.title;
    case TextModelRole:
        return notification.text;
    case ActionsModelRole:
        return notification.actions;
    case ActiveModelRole:
        return notification.active;
    default:
        return QVariant();
    }
}

NotificationDbusInterface *NotificationsModel::getNotification(const QModelIndex &index) const
{
    if (!index.isValid()) {
        return nullptr;
    }

    int row = index.row();
    if (row < 0 || row >= m_notificationList.size()) {
        return nullptr;
    }

    return m_notificationList[row].dbusInterface;
}

int NotificationsModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        // Return size 0 if we are a child because this is not a tree
        return 0;
    }

    return m_notificationList.count();
}

bool NotificationsModel::isAnyDimissable() const
{
    for (const NotificationEntry &notification : m_notificationList) {
        if (notification.active && notification.dismissable) {
            return true;
        }
    }
    return false;
}

void NotificationsModel::dismissAll()
{
    for (const NotificationEntry &notification : m_notificationList) {
        if (notification.active && notification.dismissable && notification.dbusInterface) {
            notification.dbusInterface->dismiss();
        }
    }
}

void NotificationsModel::clearNotifications()
{
    if (m_notificationList.isEmpty()) {
        return;
    }

    beginResetModel();
    for (const NotificationEntry &entry : std::as_const(m_notificationList)) {
        delete entry.dbusInterface;
    }
    m_notificationList.clear();
    endResetModel();
}

void NotificationsModel::notificationUpdated()
{
    NotificationDbusInterface *notification = qobject_cast<NotificationDbusInterface *>(sender());
    for (int row = 0; row < m_notificationList.size(); ++row) {
        if (m_notificationList[row].dbusInterface == notification) {
            Q_EMIT dataChanged(index(row, 0), index(row, 0), {IconPathModelRole});
            return;
        }
    }
}

#include "moc_notificationsmodel.cpp"
