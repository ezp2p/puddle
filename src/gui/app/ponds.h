#pragma once

#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QTimer>

#include "apiclient.h"
#include "jsonlistmodel.h"

// The Ponds the owner is in, for the rail, and the one that is open: its channels and members, and the messages of the open
// channel. Read from the owner API (/api/v1/ponds), which answers from what our Puddle keeps of each Pond; it changes a number
// (`version`) whenever that does, so the timer asks for little until something happens.
class Ponds : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *list READ list CONSTANT)           // the rail: id, name, status, address, icon (a picture's hash, "" for none)
    Q_PROPERTY(QObject *channels READ channels CONSTANT)   // of the open Pond: id, name, topic, kind (text, voice), voice (who is in it: id, name, color, avatar, muted, deafened, isMe)
    Q_PROPERTY(QObject *members READ members CONSTANT)     // of the open Pond: id, name, color, avatar, isMe, role, muted, mutedUntil
    Q_PROPERTY(QObject *bans READ bans CONSTANT)           // of the open Pond, once loadBans() has asked: member, name, by, byName, reason, ts
    Q_PROPERTY(QObject *invites READ invites CONSTANT)     // of the open Pond, once loadInvites() has asked: token, link, uses, maxUses, expires, created
    Q_PROPERTY(QObject *messages READ messages CONSTANT)   // the open channel, oldest first: id, author, name, tint, avatar, text, files, ts, mine, grouped, dayStart
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)            // the open Pond's id, "" for none
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentStatus READ currentStatus NOTIFY currentInfoChanged) // connecting, connected, unreachable, removed, not_proven
    Q_PROPERTY(QString currentDescription READ currentDescription NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentIcon READ currentIcon NOTIFY currentInfoChanged)  // its icon, a picture's hash; "" for none
    Q_PROPERTY(bool canIcon READ canIcon NOTIFY currentInfoChanged)             // it has icons (its `features` say `pictures`)
    Q_PROPERTY(QString channel READ channel NOTIFY channelChanged)            // the open channel's id, "" for none
    Q_PROPERTY(QString channelName READ channelName NOTIFY channelChanged)
    Q_PROPERTY(QString channelTopic READ channelTopic NOTIFY channelChanged)
    Q_PROPERTY(QString currentRole READ currentRole NOTIFY currentInfoChanged)   // ours there: operator, administrator or member
    Q_PROPERTY(bool canAdmin READ canAdmin NOTIFY currentInfoChanged)           // operator or administrator: Server Settings
    Q_PROPERTY(bool canMute READ canMute NOTIFY currentInfoChanged)             // ...and the Pond can mute (protocol 7)
    Q_PROPERTY(double mutedUntil READ mutedUntil NOTIFY currentInfoChanged)     // we are muted there: until when (ms), 0 until unmuted; -1 not
    Q_PROPERTY(QString currentOffer READ currentOffer NOTIFY currentInfoChanged) // a role it offers us ("operator", "administrator"), or ""
    Q_PROPERTY(QString myId READ myId NOTIFY currentInfoChanged)
    Q_PROPERTY(bool canVoice READ canVoice NOTIFY currentInfoChanged)
    // direct messages between members (docs/pond-dms.md): the open Pond passes them on; we take them there
    Q_PROPERTY(bool dmsOn READ dmsOn NOTIFY currentInfoChanged)
    Q_PROPERTY(bool acceptDms READ acceptDms NOTIFY currentInfoChanged)          // the open Pond has voice channels (its `features`)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY hasOlderChanged)
    Q_PROPERTY(bool joining READ joining NOTIFY joiningChanged)
    // The voice channel we are in (docs/ponds.md, "Voice channels"), in whichever Pond: "" for none
    Q_PROPERTY(QString voicePond READ voicePond NOTIFY voiceChanged)
    Q_PROPERTY(QString voiceChannel READ voiceChannel NOTIFY voiceChanged)
    Q_PROPERTY(QString voiceState READ voiceState NOTIFY voiceChanged)       // connecting, active, ended
    Q_PROPERTY(QString voiceReason READ voiceReason NOTIFY voiceChanged)     // ended: failed, removed
    Q_PROPERTY(QString voiceName READ voiceName NOTIFY voiceChanged)         // the channel's name
    Q_PROPERTY(QString voicePondName READ voicePondName NOTIFY voiceChanged)
    Q_PROPERTY(bool voiceMuted READ voiceMuted NOTIFY voiceChanged)
    Q_PROPERTY(bool voiceDeafened READ voiceDeafened NOTIFY voiceChanged)
    Q_PROPERTY(bool voiceCamera READ voiceCamera NOTIFY voiceChanged)       // our camera, in the voice channel
    Q_PROPERTY(bool voiceScreen READ voiceScreen NOTIFY voiceChanged)       // the screen we share there
    Q_PROPERTY(QStringList voicePeople READ voicePeople NOTIFY voiceChanged) // who is in our voice channel, by id, in the order they came (us too)
public:
    explicit Ponds(ApiClient *api, QObject *parent = nullptr);
    bool dmsOn() const { return info_.value("dms").toObject().value("on").toBool(); }
    bool acceptDms() const { return people_.value(me_).value("dms").toBool(true); }
    // people the owner blocked (Chats): what they write in Ponds isn't shown
    void setBlocked(const QSet<QString> &ids) { if (ids != blocked_) { blocked_ = ids; rebuildMessages(); } }

    QObject *list() { return &list_; }
    QObject *channels() { return &channels_; }
    QObject *members() { return &members_; }
    QObject *bans() { return &bans_; }
    QObject *invites() { return &invites_; }
    QString currentRole() const { return info_.value("role").toString(QStringLiteral("member")); }
    bool canAdmin() const { return currentRole() == QLatin1String("operator") || currentRole() == QLatin1String("administrator"); }
    bool canMute() const { return canAdmin() && info_.value("protocol").toInt() >= 7; }
    double mutedUntil() const { return people_.value(me_).value("mutedUntil").toDouble(-1); }
    QString currentOffer() const { return info_.value("offer").toObject().value("role").toString(); }
    QString myId() const { return me_; }
    bool canVoice() const { return info_.value("features").toArray().contains(QStringLiteral("voice")); }
    QObject *messages() { return &messages_; }
    QString current() const { return current_; }
    QString currentName() const { return info_.value("name").toString(); }
    QString currentStatus() const { return info_.value("status").toString(); }
    QString currentDescription() const { return info_.value("description").toString(); }
    QString currentIcon() const { return info_.value("icon").toObject().value("hash").toString(); }
    bool canIcon() const { return info_.value("features").toArray().contains(QStringLiteral("pictures")); }
    QString channel() const { return channel_; }
    QString channelName() const;
    QString channelTopic() const;
    bool hasOlder() const { return hasOlder_; }
    bool joining() const { return joining_; }
    QString voicePond() const { return voice_.value("pond").toString(); }
    QString voiceChannel() const { return voice_.isEmpty() ? QString() : QString::number(qint64(voice_.value("channel").toDouble())); }
    QString voiceState() const { return voice_.value("state").toString(); }
    QString voiceReason() const { return voice_.value("reason").toString(); }
    QString voiceName() const { return channelNames_.value(voicePond()).value(voiceChannel()); }
    QString voicePondName() const { return pondNames_.value(voicePond()); }
    bool voiceMuted() const { return voice_.value("muted").toBool(); }
    bool voiceDeafened() const { return voice_.value("deafened").toBool(); }
    bool voiceCamera() const { return voice_.value("camera").toBool(); }
    bool voiceScreen() const { return voice_.value("screen").toBool(); }
    QStringList voicePeople() const { return voicePeople_; }

    void start() { timer_.start(); refresh(); }
    void stop() { timer_.stop(); }
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
    // A member of the open Pond's name, as they chose to appear there (their id's start if they show nothing)
    Q_INVOKABLE QString nameOf(const QString &memberId) const { return people_.value(memberId).value("name").toString(memberId.left(8)); }
    // A member's card (a click on their name or picture): id, name, avatar, blurb, pronouns, colors, angle, role, isMe, muted,
    // mutedUntil (ms; 0 until unmuted)
    Q_INVOKABLE QVariantMap card(const QString &memberId) const;
    // The parts of a profile a card shows: name, avatar, blurb, pronouns, colors (its name colour, or the stops of its gradient), angle
    static QVariantMap cardOf(const QJsonObject &profile);
    // ...and their picture's hash there ("" for none)
    // A Pond's icon (a picture's hash, "" for none), from the rail's list: for its badge on someone met through it
    QString iconOf(const QString &pondId) const;
    Q_INVOKABLE QString avatarOf(const QString &memberId) const { return people_.value(memberId).value("avatar").toString(); }

    // ---- roles (the Pond's docs/design.md, "Roles") ----
    Q_INVOKABLE void answerOffer(bool accept);
    // What an operator or administrator does, in Server Settings. Each says how it went with adminDone or adminFailed.
    Q_INVOKABLE void saveOverview(const QString &name, const QString &description);
    // The Pond's icon: a picture our Puddle holds (Chats.uploadPicture), or "" to have none
    Q_INVOKABLE void setIcon(const QString &hash);
    // `kind`: "text" or "voice" (a voice channel, where the Pond has them)
    Q_INVOKABLE void createChannel(const QString &name, const QString &topic, const QString &kind = QStringLiteral("text"));
    Q_INVOKABLE void editChannel(const QString &channelId, const QString &name, const QString &topic);
    Q_INVOKABLE void deleteChannel(const QString &channelId);
    Q_INVOKABLE void kick(const QString &memberId);
    // `seconds` 0: until unmuted
    Q_INVOKABLE void setAcceptDms(bool accept); // in the open Pond
    Q_INVOKABLE void mute(const QString &memberId, int seconds);
    Q_INVOKABLE void unmute(const QString &memberId);
    Q_INVOKABLE void ban(const QString &memberId, const QString &reason);
    Q_INVOKABLE void unban(const QString &memberId);
    Q_INVOKABLE void loadBans();
    // Invites (the Pond's docs/design.md, "Invite links"). `uses` 0: no limit; `seconds` 0: never expires. inviteMade(link) says the new one's link.
    Q_INVOKABLE void loadInvites();
    Q_INVOKABLE void createInvite(int uses, int seconds);
    Q_INVOKABLE void revokeInvite(const QString &token);
    // May we act on this member from the app? (Never ourselves or an operator, and only a lower rank.)
    Q_INVOKABLE bool canActOn(const QString &memberId) const;

    // ---- voice channels (docs/ponds.md, "Voice channels") ----
    Q_INVOKABLE void joinVoice(const QString &channelId);   // in the open Pond; says why not with failed()
    Q_INVOKABLE void leaveVoice();
    Q_INVOKABLE void setVoiceMuted(bool muted);
    Q_INVOKABLE void setVoiceDeafened(bool deafened);
    Q_INVOKABLE void setVoiceCamera(bool on);
    Q_INVOKABLE void setVoiceScreen(bool on);
    Q_INVOKABLE void refreshVoice();

signals:
    void currentChanged();
    void currentInfoChanged();
    void channelChanged();
    void hasOlderChanged();
    void joiningChanged();
    void joined(const QString &pondId);
    void voiceChanged();
    void failed(const QString &what);
    void adminDone(const QString &what);   // said in words, for a short notice
    void adminFailed(const QString &why);
    void inviteMade(const QString &link);
    void listChanged();   // the rail's list was read again (names, icons)

private:
    QSet<QString> blocked_;
    void refreshPond();
    void refreshMessages();
    void rebuildMessages();
    QString pathOfChannel() const;
    QString why(int status, const QJsonDocument &json, const QString &doing) const;
    void takeVoice(const QJsonObject &voice);
    void setVoiceFlag(const QString &name, bool on);
    QJsonObject channelRow(const QJsonObject &c) const;
    void admin(const QString &path, const QJsonObject &body, const QString &done, const QString &doing, bool reloadBans = false,
               bool reloadInvites = false);

    ApiClient *api_;
    QTimer timer_;
    JsonListModel list_, channels_, members_, messages_, bans_, invites_;
    qint64 version_ = -1;
    QString current_, channel_, me_;
    QJsonObject info_;
    QHash<QString, QString> lastChannel_;          // pond id -> the channel open there last
    QHash<QString, QJsonObject> people_;           // member id -> {name, color, avatar}
    QJsonObject voice_;                            // GET /api/v1/voice's `voice`, or empty
    QHash<QString, QHash<QString, QString>> channelNames_; // pond id -> channel id -> name, of every Pond (for the voice bar)
    QHash<QString, QString> pondNames_;
    bool busyVoice_ = false;
    QStringList voicePeople_;
    QHash<QString, QHash<QString, QStringList>> voiceIn_; // pond id -> channel id -> who is in it (by slot), from the list
    void updateVoicePeople();
    int voiceGen_ = 0;                             // the owner changed something since a request for it went out: drop its answer
    QList<QJsonObject> loaded_;                    // the open channel's messages, oldest first, as the API gives them
    bool hasOlder_ = false, joining_ = false, busyList_ = false, busyMessages_ = false, busyOlder_ = false;
    int generation_ = 0;                           // a channel opened since a request went out: drop its answer
};
