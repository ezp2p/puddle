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
    Q_PROPERTY(QObject *channels READ channels CONSTANT)   // of the open Pond, in its order: id, name, topic, kind (text, voice), voice (who is in it: id, name, color, avatar, muted, deafened, isMe), restricted, roles (ids)
    Q_PROPERTY(QObject *members READ members CONSTANT)     // of the open Pond: id, name, color, avatar, isMe, role, muted, mutedUntil, roles (ids), group (the heading they are under: their highest role shown apart, or "Members"; "" when no role is shown apart)
    Q_PROPERTY(QObject *roles READ roles CONSTANT)         // the open Pond's roles, highest first: id, name, count (members holding it), color ("" none), hoist, permissions (names), editable (we may change, give and move it)
    Q_PROPERTY(QObject *pages READ pages CONSTANT)         // the open Pond's pages, in its order: id, title, kind, updated (the Pond's docs/design.md, "Pages")
    Q_PROPERTY(QString page READ page NOTIFY pageChanged)  // the open page's id; "" while a channel is shown
    // the open page as the Pond gives it: id, title, kind, body, markdown (the body with #channel-name as links to "channel:<id>"),
    // files (as a message's), updated, updatedBy (a name); empty until it comes
    Q_PROPERTY(QVariantMap pageData READ pageData NOTIFY pageDataChanged)
    Q_PROPERTY(bool canPages READ canPages NOTIFY currentInfoChanged)           // it has pages and a banner (`pages`)
    Q_PROPERTY(bool canManagePages READ canManagePages NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentBanner READ currentBanner NOTIFY currentInfoChanged) // its banner, a picture's hash; "" for none
    // names in a role's colour (protocol 12): each member's choice, or the Pond's for everyone; our choice, and our role's colour
    Q_PROPERTY(bool canRoleColors READ canRoleColors NOTIFY currentInfoChanged)
    Q_PROPERTY(bool roleColorsEnforced READ roleColorsEnforced NOTIFY currentInfoChanged)
    Q_PROPERTY(bool useRoleColor READ useRoleColor NOTIFY currentInfoChanged)
    Q_PROPERTY(QString myRoleColor READ myRoleColor NOTIFY currentInfoChanged)
    Q_PROPERTY(QObject *bans READ bans CONSTANT)           // of the open Pond, once loadBans() has asked: member, name, by, byName, reason, ts
    Q_PROPERTY(QObject *invites READ invites CONSTANT)     // of the open Pond, once loadInvites() has asked: token, link, uses, maxUses, expires, created
    Q_PROPERTY(QObject *messages READ messages CONSTANT)   // the open channel, oldest first: id, author, name, tint, avatar, text, files, ts, mine, grouped, dayStart
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)            // the open Pond's id, "" for none
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentStatus READ currentStatus NOTIFY currentInfoChanged) // connecting, connected, unreachable, removed, not_proven
    Q_PROPERTY(QString currentDescription READ currentDescription NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentIcon READ currentIcon NOTIFY currentInfoChanged)  // its icon, a picture's hash; "" for none
    Q_PROPERTY(bool canIcon READ canIcon NOTIFY currentInfoChanged)             // it has icons (its `features` say `pictures`)
    Q_PROPERTY(bool canRoles READ canRoles NOTIFY currentInfoChanged)           // it has roles for channels and their order (`channel_roles`)
    Q_PROPERTY(QString channel READ channel NOTIFY channelChanged)            // the open channel's id, "" for none
    Q_PROPERTY(QString channelName READ channelName NOTIFY channelChanged)
    Q_PROPERTY(QString channelTopic READ channelTopic NOTIFY channelChanged)
    Q_PROPERTY(QString currentRole READ currentRole NOTIFY currentInfoChanged)   // ours there: operator, administrator or member
    Q_PROPERTY(bool canAdmin READ canAdmin NOTIFY currentInfoChanged)           // operator or administrator: Server Settings
    Q_PROPERTY(bool canMute READ canMute NOTIFY currentInfoChanged)             // we may mute (protocol 7)
    // What we may do there (the Pond's docs/design.md, "Permissions"): an operator or administrator all of it, a member what
    // their roles let them. canAdmin is any of it: Server Settings.
    Q_PROPERTY(bool canManagePond READ canManagePond NOTIFY currentInfoChanged)
    Q_PROPERTY(bool canManageChannels READ canManageChannels NOTIFY currentInfoChanged)
    Q_PROPERTY(bool canManageRoles READ canManageRoles NOTIFY currentInfoChanged)
    Q_PROPERTY(bool canManageInvites READ canManageInvites NOTIFY currentInfoChanged)
    Q_PROPERTY(bool canKick READ canKick NOTIFY currentInfoChanged)
    Q_PROPERTY(bool canBan READ canBan NOTIFY currentInfoChanged)
    Q_PROPERTY(bool canPermissions READ canPermissions NOTIFY currentInfoChanged) // roles have permissions, colours and an order there (`role_permissions`)
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
    QObject *roles() { return &roles_; }
    QObject *pages() { return &pages_; }
    QString page() const { return page_; }
    QVariantMap pageData() const { return pageData_; }
    bool canPages() const { return info_.value("features").toArray().contains(QStringLiteral("pages")); }
    bool canManagePages() const { return canPages() && may("manage_pages"); }
    QString currentBanner() const { return info_.value("banner").toObject().value("hash").toString(); }
    bool canRoleColors() const { return info_.value("features").toArray().contains(QStringLiteral("role_colors")); }
    bool roleColorsEnforced() const { return info_.value("role_colors").toString() == QLatin1String("enforced"); }
    bool useRoleColor() const;
    QString myRoleColor() const;
    QObject *bans() { return &bans_; }
    QObject *invites() { return &invites_; }
    QString currentRole() const { return info_.value("role").toString(QStringLiteral("member")); }
    bool ranked() const { return currentRole() == QLatin1String("operator") || currentRole() == QLatin1String("administrator"); }
    bool may(const char *permission) const { return ranked() || myPermissions().contains(QLatin1String(permission)); }
    bool canAdmin() const { return ranked() || !myPermissions().isEmpty(); }
    bool canMute() const { return may("mute_members") && info_.value("protocol").toInt() >= 7; }
    bool canManagePond() const { return may("manage_pond"); }
    bool canManageChannels() const { return may("manage_channels"); }
    bool canManageRoles() const { return may("manage_roles"); }
    bool canManageInvites() const { return may("manage_invites"); }
    bool canKick() const { return may("kick_members"); }
    bool canBan() const { return may("ban_members"); }
    bool canPermissions() const { return info_.value("features").toArray().contains(QStringLiteral("role_permissions")); }
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
    bool canRoles() const { return info_.value("features").toArray().contains(QStringLiteral("channel_roles")); }
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
    // roles for channels, and the channels' order (the Pond's docs/design.md, "Roles for channels")
    // ---- pages (the Pond's docs/design.md, "Pages") ----
    Q_INVOKABLE void openPage(const QString &pageId);       // shown instead of the channel, until a channel is opened
    Q_INVOKABLE void createPage(const QString &title);      // opened once made, to be written
    // its title, its markdown, and the files it carries: those it had ({id, name, mime, size, ...}) and new ones from the composer
    Q_INVOKABLE void savePage(const QString &pageId, const QString &title, const QString &body, const QVariantList &files);
    Q_INVOKABLE void deletePage(const QString &pageId);
    Q_INVOKABLE void movePage(const QString &pageId, int by); // -1 up, +1 down
    Q_INVOKABLE void fetchPageFile(const QString &fileId);   // one of the open page's that did not come by itself
    Q_INVOKABLE void setRoleColorsEnforced(bool on);       // those who may manage the Pond
    Q_INVOKABLE void setUseRoleColor(bool on);             // our own name, in this Pond
    Q_INVOKABLE void setBanner(const QString &hash);        // a picture sent to our Puddle first; "" for none
    // a page's markdown as shown: each #channel-name (a channel we see) a link to "channel:<id>", outside code
    Q_INVOKABLE QString pageMarkdown(const QString &body) const;
    Q_INVOKABLE void createRole(const QString &name);
    Q_INVOKABLE void renameRole(const QString &roleId, const QString &name);
    // change what is in `changes`: name, color ("#rrggbb", "" for none), hoist, permissions (names)
    Q_INVOKABLE void editRole(const QString &roleId, const QVariantMap &changes);
    Q_INVOKABLE void moveRole(const QString &roleId, int by); // -1 up, +1 down
    // the roles a member holds, highest first: id, name, color
    Q_INVOKABLE QVariantList rolesOf(const QString &memberId) const;
    Q_INVOKABLE bool roleEditableAt(int row) const { return roles_.items().value(row).value("editable").toBool(); }
    // may we do this there (a permission's name: manage_roles, kick_members, ...)? Every one, with a rank
    Q_INVOKABLE bool hasPermission(const QString &permission) const { return ranked() || myPermissions().contains(permission); }
    Q_INVOKABLE void deleteRole(const QString &roleId);
    Q_INVOKABLE QString roleName(const QString &roleId) const;
    Q_INVOKABLE void setMemberRole(const QString &memberId, const QString &roleId, bool held); // give or take one role
    Q_INVOKABLE void setChannelRoles(const QString &channelId, bool everyone, const QStringList &roleIds);
    Q_INVOKABLE void moveChannel(const QString &channelId, int by); // -1 up, +1 down, for everyone
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
    // May we act on this member from the app? (Never ourselves or an operator, and only a lower rank; with permissions from
    // roles, only a member whose highest role is below ours.) Which actions, canKick, canBan and canMute say.
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
    void pageChanged();
    void pageDataChanged();
    void pageSaved();     // a save of the open page went through

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
    QSet<QString> myPermissions() const;           // what our roles let us do there (not counting rank)
    int topOf(const QString &memberId) const;      // where their highest role is in the order: 0 the highest; INT_MAX none
    QString memberRole(const QString &memberId) const;
    void loadPage();
    void closePage();
    QString page_;
    QVariantMap pageData_;
    int pageGen_ = 0, pageTicks_ = 0;
    void admin(const QString &path, const QJsonObject &body, const QString &done, const QString &doing, bool reloadBans = false,
               bool reloadInvites = false);

    ApiClient *api_;
    QTimer timer_;
    JsonListModel list_, channels_, members_, messages_, bans_, invites_, roles_, pages_;
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
