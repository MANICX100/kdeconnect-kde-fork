/**
 * SPDX-FileCopyrightText: 2013 Albert Vaca <albertvaka@gmail.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
 */

#include "notificationsplugin.h"

#include "networkpacket.h"
#include "notification.h"
#include "plugin_notifications_debug.h"
#include "sendreplydialog.h"
#include <dbushelper.h>

#include <KPluginFactory>
#include <KSystemClipboard>

#if WITH_X11
#include <KStartupInfo>
#include <private/qtx11extras_p.h>
#endif

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMimeData>
#include <QSet>

K_PLUGIN_CLASS_WITH_JSON(NotificationsPlugin, "kdeconnect_notifications.json")

namespace
{
constexpr qsizetype maximumNotificationHistorySize = 100;
}

NotificationsPlugin::NotificationsPlugin(QObject *parent, const QVariantList &args)
    : KdeConnectPlugin(parent, args)
{
    loadNotificationHistory();
}

NotificationsPlugin::~NotificationsPlugin()
{
    markAllNotificationHistoryInactive();
}

void NotificationsPlugin::connected()
{
    NetworkPacket np(PACKET_TYPE_NOTIFICATION_REQUEST, {{QStringLiteral("request"), true}});
    sendPacket(np);
}

void NotificationsPlugin::receivePacket(const NetworkPacket &np)
{
    if (np.get<bool>(QStringLiteral("request"))) {
        qCWarning(KDECONNECT_PLUGIN_NOTIFICATIONS) << "Unexpected notification request. Maybe the paired client is very old?";
    }

    if (np.get<bool>(QStringLiteral("isCancel"))) {
        QString id = np.get<QString>(QStringLiteral("id"));
        // cut off kdeconnect-android's prefix if there:
        if (id.startsWith(QLatin1String("org.kde.kdeconnect_tp::")))
            id = id.mid(id.indexOf(QLatin1String("::")) + 2);
        removeNotification(id);
        return;
    }

    QString id = np.get<QString>(QStringLiteral("id"));

    Notification *noti = nullptr;

    if (!m_internalIdToPublicId.contains(id)) {
        noti = new Notification(np, device(), this);

        if (noti->isReady()) {
            addNotification(noti);
        } else {
            connect(noti, &Notification::ready, this, &NotificationsPlugin::notificationReady);
        }
    } else {
        QString pubId = m_internalIdToPublicId.value(id);
        noti = m_notifications.value(pubId);
        noti->update(np);
        upsertNotificationHistory(noti, pubId);
    }
}

void NotificationsPlugin::clearNotifications()
{
    qDeleteAll(m_notifications);
    m_notifications.clear();
    Q_EMIT allNotificationsRemoved();
}

QStringList NotificationsPlugin::activeNotifications()
{
    return m_notifications.keys();
}

void NotificationsPlugin::notificationReady()
{
    Notification *noti = static_cast<Notification *>(sender());
    disconnect(noti, &Notification::ready, this, &NotificationsPlugin::notificationReady);
    addNotification(noti);
}

void NotificationsPlugin::addNotification(Notification *noti)
{
    const QString &internalId = noti->internalId();

    if (m_internalIdToPublicId.contains(internalId)) {
        removeNotification(internalId);
    }

    // qCDebug(KDECONNECT_PLUGIN_NOTIFICATIONS) << "addNotification" << internalId;

    connect(noti, &Notification::dismissRequested, this, &NotificationsPlugin::dismissRequested);

    connect(noti, &Notification::replyRequested, this, [this, noti] {
        replyRequested(noti);
    });

    connect(noti, &Notification::actionTriggered, this, &NotificationsPlugin::sendAction);
    connect(noti, &Notification::replied, this, [this, noti](const QString &message) {
        sendReply(noti->replyId(), message);
    });

    const QString &publicId = newId();
    m_notifications[publicId] = noti;
    m_internalIdToPublicId[internalId] = publicId;

    QDBusConnection::sessionBus().registerObject(device()->dbusPath() + QStringLiteral("/notifications/") + publicId,
                                                 noti,
                                                 QDBusConnection::ExportScriptableContents);
    upsertNotificationHistory(noti, publicId);
    Q_EMIT notificationPosted(publicId);
}

void NotificationsPlugin::removeNotification(const QString &internalId)
{
    // qCDebug(KDECONNECT_PLUGIN_NOTIFICATIONS) << "removeNotification" << internalId;

    markNotificationHistoryInactive(internalId);

    if (!m_internalIdToPublicId.contains(internalId)) {
        qCDebug(KDECONNECT_PLUGIN_NOTIFICATIONS) << "Not found noti by internal Id: " << internalId;
        return;
    }

    QString publicId = m_internalIdToPublicId.take(internalId);

    Notification *noti = m_notifications.take(publicId);
    if (!noti) {
        qCDebug(KDECONNECT_PLUGIN_NOTIFICATIONS) << "Not found noti by public Id: " << publicId;
        return;
    }

    // Deleting the notification will unregister it automatically
    noti->deleteLater();

    Q_EMIT notificationRemoved(publicId);
}

void NotificationsPlugin::dismissRequested(const QString &internalId)
{
    NetworkPacket np(PACKET_TYPE_NOTIFICATION_REQUEST);
    np.set<QString>(QStringLiteral("cancel"), internalId);
    sendPacket(np);

    // Workaround: we erase notifications without waiting a response from the
    // phone because we won't receive a response if we are out of sync and this
    // notification no longer exists. Ideally, each time we reach the phone
    // after some time disconnected we should re-sync all the notifications.
    removeNotification(internalId);
}

void NotificationsPlugin::replyRequested(Notification *noti)
{
    QString replyId = noti->replyId();
    QString appName = noti->appName();
    QString originalMessage = noti->ticker();
    SendReplyDialog *dialog = new SendReplyDialog(originalMessage, replyId, appName);
    connect(dialog, &SendReplyDialog::sendReply, this, &NotificationsPlugin::sendReply);
    dialog->show();
#if WITH_X11
    auto window = qobject_cast<QWindow *>(dialog->windowHandle());
    if (window && QX11Info::isPlatformX11()) {
        KStartupInfo::setNewStartupId(window, QX11Info::nextStartupId());
    }
#endif
    dialog->raise();
}

void NotificationsPlugin::sendReply(const QString &replyId, const QString &message)
{
    NetworkPacket np(PACKET_TYPE_NOTIFICATION_REPLY);
    np.set<QString>(QStringLiteral("requestReplyId"), replyId);
    np.set<QString>(QStringLiteral("message"), message);
    sendPacket(np);
}

void NotificationsPlugin::sendAction(const QString &key, const QString &action)
{
    NetworkPacket np(PACKET_TYPE_NOTIFICATION_ACTION);
    np.set<QString>(QStringLiteral("key"), key);
    np.set<QString>(QStringLiteral("action"), action);
    sendPacket(np);

    copyAuthCodeIfPresent(action);
}

void NotificationsPlugin::copyAuthCodeIfPresent(const QString &action)
{
    // The auth code we receive has invisible characters in it for some reason.
    // (U+2063 INVISIBLE SEPARATOR between each digit).
    // Remove them if present before continuing.
    QString sanitizedAction = action;
    sanitizedAction.remove(QChar(0x2063));

    // Match blocks of digits, 4-10 digits long. This should match auth codes
    // in any language without relying on the action text having a specific
    // keyword in it such as "Copy" in English.
    QRegularExpression authCodeRegex(QStringLiteral("\\b(\\d{4,10})\\b"));
    QRegularExpressionMatch match = authCodeRegex.match(sanitizedAction);

    if (!match.hasMatch()) {
        return;
    }

    QString text = match.captured(1);
    auto mimeData = new QMimeData;
    mimeData->setText(text);
    KSystemClipboard::instance()->setMimeData(mimeData, QClipboard::Clipboard);
}

QString NotificationsPlugin::newId()
{
    return QString::number(++m_lastId);
}

QString NotificationsPlugin::notificationHistory() const
{
    return QString::fromUtf8(m_persistedNotificationHistory);
}

void NotificationsPlugin::loadNotificationHistory()
{
    const QByteArray storedHistory = config()->getByteArray(QStringLiteral("history"), QByteArrayLiteral("[]"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(storedHistory, &parseError);
    QSet<QString> internalIds;

    if (parseError.error == QJsonParseError::NoError && document.isArray()) {
        const QJsonArray storedArray = document.array();
        for (const QJsonValue &value : storedArray) {
            if (!value.isObject() || m_notificationHistory.size() >= maximumNotificationHistorySize) {
                continue;
            }

            const QJsonObject storedNotification = value.toObject();
            const QString internalId = storedNotification.value(QStringLiteral("internalId")).toString();
            if (internalId.isEmpty() || internalIds.contains(internalId)) {
                continue;
            }

            QJsonArray actions;
            const QJsonArray storedActions = storedNotification.value(QStringLiteral("actions")).toArray();
            for (const QJsonValue &action : storedActions) {
                if (action.isString()) {
                    actions.append(action.toString());
                }
            }

            QJsonObject notification{
                {QStringLiteral("actions"), actions},
                {QStringLiteral("internalId"), internalId},
                {QStringLiteral("publicId"), QString()},
                {QStringLiteral("appName"), storedNotification.value(QStringLiteral("appName")).toString()},
                {QStringLiteral("ticker"), storedNotification.value(QStringLiteral("ticker")).toString()},
                {QStringLiteral("title"), storedNotification.value(QStringLiteral("title")).toString()},
                {QStringLiteral("text"), storedNotification.value(QStringLiteral("text")).toString()},
                {QStringLiteral("dismissable"), storedNotification.value(QStringLiteral("dismissable")).toBool()},
                {QStringLiteral("repliable"), storedNotification.value(QStringLiteral("repliable")).toBool()},
                {QStringLiteral("active"), false},
                {QStringLiteral("timestamp"), storedNotification.value(QStringLiteral("timestamp")).toDouble()},
            };
            m_notificationHistory.append(notification);
            internalIds.insert(internalId);
        }
    }

    m_persistedNotificationHistory = QJsonDocument(m_notificationHistory).toJson(QJsonDocument::Compact);
    if (m_persistedNotificationHistory != storedHistory) {
        config()->set(QStringLiteral("history"), m_persistedNotificationHistory);
        Q_EMIT notificationHistoryChanged();
    }
}

void NotificationsPlugin::persistNotificationHistory()
{
    const QByteArray history = QJsonDocument(m_notificationHistory).toJson(QJsonDocument::Compact);
    if (history == m_persistedNotificationHistory) {
        return;
    }

    m_persistedNotificationHistory = history;
    config()->set(QStringLiteral("history"), history);
    Q_EMIT notificationHistoryChanged();
}

void NotificationsPlugin::upsertNotificationHistory(Notification *noti, const QString &publicId)
{
    const QString internalId = noti->internalId();
    for (qsizetype i = m_notificationHistory.size() - 1; i >= 0; --i) {
        if (m_notificationHistory.at(i).toObject().value(QStringLiteral("internalId")).toString() == internalId) {
            m_notificationHistory.removeAt(i);
        }
    }

    QJsonObject notification{
        {QStringLiteral("actions"), QJsonArray::fromStringList(noti->actions())},
        {QStringLiteral("internalId"), internalId},
        {QStringLiteral("publicId"), publicId},
        {QStringLiteral("appName"), noti->appName()},
        {QStringLiteral("ticker"), noti->ticker()},
        {QStringLiteral("title"), noti->title()},
        {QStringLiteral("text"), noti->text()},
        {QStringLiteral("dismissable"), noti->dismissable()},
        {QStringLiteral("repliable"), !noti->replyId().isEmpty()},
        {QStringLiteral("active"), true},
        {QStringLiteral("timestamp"), static_cast<double>(QDateTime::currentMSecsSinceEpoch())},
    };
    m_notificationHistory.prepend(notification);
    while (m_notificationHistory.size() > maximumNotificationHistorySize) {
        m_notificationHistory.removeLast();
    }
    persistNotificationHistory();
}

void NotificationsPlugin::markNotificationHistoryInactive(const QString &internalId)
{
    for (qsizetype i = 0; i < m_notificationHistory.size(); ++i) {
        QJsonObject notification = m_notificationHistory.at(i).toObject();
        if (notification.value(QStringLiteral("internalId")).toString() != internalId) {
            continue;
        }
        if (!notification.value(QStringLiteral("active")).toBool() && notification.value(QStringLiteral("publicId")).toString().isEmpty()) {
            return;
        }

        notification.insert(QStringLiteral("active"), false);
        notification.insert(QStringLiteral("publicId"), QString());
        m_notificationHistory.replace(i, notification);
        persistNotificationHistory();
        return;
    }
}

void NotificationsPlugin::markAllNotificationHistoryInactive()
{
    bool changed = false;
    for (qsizetype i = 0; i < m_notificationHistory.size(); ++i) {
        QJsonObject notification = m_notificationHistory.at(i).toObject();
        if (!notification.value(QStringLiteral("active")).toBool() && notification.value(QStringLiteral("publicId")).toString().isEmpty()) {
            continue;
        }

        notification.insert(QStringLiteral("active"), false);
        notification.insert(QStringLiteral("publicId"), QString());
        m_notificationHistory.replace(i, notification);
        changed = true;
    }

    if (changed) {
        persistNotificationHistory();
    }
}

QString NotificationsPlugin::dbusPath() const
{
    return QLatin1String("/modules/kdeconnect/devices/%1/notifications").arg(device()->id());
}

#include "moc_notificationsplugin.cpp"
#include "notificationsplugin.moc"
