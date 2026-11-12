#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>

#include "apiclient.h"
#include "jsonlistmodel.h"

// The Ponds the owner is in, for the rail, and the one that is open: its channels and members, and the messages of the open
// channel. Read from the owner API (/api/v1/ponds), which answers from what our Puddle keeps of each Pond; it changes a number
// (`version`) whenever that does, so the timer asks for little until something happens.
class Ponds : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *list READ list CONSTANT)           // the rail: id, name, status, address
    Q_PROPERTY(QObject *channels READ channels CONSTANT)   // of the open Pond: id, name, topic
    Q_PROPERTY(QObject *members READ members CONSTANT)     // of the open Pond: id, name, color, isMe, role
    Q_PROPERTY(QObject *bans READ bans CONSTANT)           // of the open Pond, once loadBans() has asked: member, name, by, byName, reason, ts
    Q_PROPERTY(QObject *messages READ messages CONSTANT)   // the open channel, oldest first: id, author, name, tint, text, files, ts, mine, grouped, dayStart
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)            // the open Pond's id, "" for none
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentStatus READ currentStatus NOTIFY currentInfoChanged) // connecting, connected, unreachable, removed, certificate_changed
    Q_PROPERTY(QString currentDescription READ currentDescription NOTIFY currentInfoChanged)
    Q_PROPERTY(QString channel READ channel NOTIFY channelChanged)            // the open channel's id, "" for none
    Q_PROPERTY(QString channelName READ channelName NOTIFY channelChanged)
    Q_PROPERTY(QString channelTopic READ channelTopic NOTIFY channelChanged)
    Q_PROPERTY(QString currentRole READ currentRole NOTIFY currentInfoChanged)   // ours there: operator, administrator or member
    Q_PROPERTY(bool canAdmin READ canAdmin NOTIFY currentInfoChanged)           // operator or administrator: Server Settings
    Q_PROPERTY(QString currentOffer READ currentOffer NOTIFY currentInfoChanged) // a role it offers us ("operator", "administrator"), or ""
    Q_PROPERTY(QString myId READ myId NOTIFY currentInfoChanged)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY hasOlderChanged)
    Q_PROPERTY(bool joining READ joining NOTIFY joiningChanged)
public:
    explicit Ponds(ApiClient *api, QObject *parent = nullptr);

    QObject *list() { return &list_; }
    QObject *channels() { return &channels_; }
    QObject *members() { return &members_; }
    QObject *bans() { return &bans_; }
    QString currentRole() const { return info_.value("role").toString(QStringLiteral("member")); }
    bool canAdmin() const { return currentRole() == QLatin1String("operator") || currentRole() == QLatin1String("administrator"); }
    QString currentOffer() const { return info_.value("offer").toObject().value("role").toString(); }
    QString myId() const { return me_; }
    QObject *messages() { return &messages_; }
    QString current() const { return current_; }
    QString currentName() const { return info_.value("name").toString(); }
    QString currentStatus() const { return info_.value("status").toString(); }
    QString currentDescription() const { return info_.value("description").toString(); }
    QString channel() const { return channel_; }
    QString channelName() const;
    QString channelTopic() const;
    bool hasOlder() const { return hasOlder_; }
    bool joining() const { return joining_; }

    void start() { timer_.start(); refresh(); }
    void setInterval(int ms) { timer_.setInterval(ms); }

    Q_INVOKABLE void refresh();
    // `profile`: which of the owner's profiles the members will see; empty for none
    Q_INVOKABLE void join(const QString &link, const QString &profile);
    Q_INVOKABLE void open(const QString &pondId);            // its first channel, or the one open there last
    Q_INVOKABLE void openChannel(const QString &channelId);
    Q_INVOKABLE void close();
    Q_INVOKABLE void send(const QString &text);              // to the open channel
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE QString sendPath() const;                   // the open channel's send route (for sending files)
    Q_INVOKABLE void fetchFile(const QString &messageId, const QString &fileId); // one that did not come by itself
    Q_INVOKABLE void leave(const QString &pondId);
    Q_INVOKABLE void setProfile(const QString &pondId, const QString &profile);
    Q_INVOKABLE QString nameAt(int row) const { return list_.at(row).value("name").toString(); }

    // ---- roles (the Pond's docs/design.md, "Roles") ----
    Q_INVOKABLE void answerOffer(bool accept);
    // What an operator or administrator does, in Server Settings. Each says how it went with adminDone or adminFailed.
    Q_INVOKABLE void saveOverview(const QString &name, const QString &description);
    Q_INVOKABLE void createChannel(const QString &name, const QString &topic);
    Q_INVOKABLE void editChannel(const QString &channelId, const QString &name, const QString &topic);
    Q_INVOKABLE void deleteChannel(const QString &channelId);
    Q_INVOKABLE void kick(const QString &memberId);
    Q_INVOKABLE void ban(const QString &memberId, const QString &reason);
    Q_INVOKABLE void unban(const QString &memberId);
    Q_INVOKABLE void loadBans();
    // May we act on this member from the app? (Never ourselves or an operator, and only a lower rank.)
    Q_INVOKABLE bool canActOn(const QString &memberId) const;

signals:
    void currentChanged();
    void currentInfoChanged();
    void channelChanged();
    void hasOlderChanged();
    void joiningChanged();
    void joined(const QString &pondId);
    void failed(const QString &what);
    void adminDone(const QString &what);   // said in words, for a short notice
    void adminFailed(const QString &why);

private:
    void refreshPond();
    void refreshMessages();
    void rebuildMessages();
    QString pathOfChannel() const;
    QString why(int status, const QJsonDocument &json, const QString &doing) const;
    void admin(const QString &path, const QJsonObject &body, const QString &done, const QString &doing, bool reloadBans = false);

    ApiClient *api_;
    QTimer timer_;
    JsonListModel list_, channels_, members_, messages_, bans_;
    qint64 version_ = -1;
    QString current_, channel_, me_;
    QJsonObject info_;
    QHash<QString, QString> lastChannel_;          // pond id -> the channel open there last
    QHash<QString, QJsonObject> people_;           // member id -> {name, color}
    QList<QJsonObject> loaded_;                    // the open channel's messages, oldest first, as the API gives them
    bool hasOlder_ = false, joining_ = false, busyList_ = false, busyMessages_ = false, busyOlder_ = false;
    int generation_ = 0;                           // a channel opened since a request went out: drop its answer
};
