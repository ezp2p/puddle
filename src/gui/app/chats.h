#pragma once

#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <functional>
#include <QTimer>
#include <memory>

#include "apiclient.h"
#include "jsonlistmodel.h"
#include "ponds.h"

// What the window shows, kept fresh from the owner API of our own Puddle: the people (friends and requests), the
// conversations with their unread counts, and the messages of the chat that is open. All of it is read over the
// API, so the same code serves a Puddle running elsewhere.
class Chats : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject *people READ people CONSTANT)               // every stream: roles below
    Q_PROPERTY(QObject *conversations READ conversations CONSTANT)  // newest first
    Q_PROPERTY(QObject *sidebar READ sidebar CONSTANT)              // the one list: what is waiting for an answer, then every friend and group (see rebuildSidebar)
    Q_PROPERTY(QObject *messages READ messages CONSTANT)            // the open chat, oldest first
    Q_PROPERTY(QObject *blocked READ blocked CONSTANT)              // people we blocked: user, name, created_at
    Q_PROPERTY(QObject *groupInvites READ groupInvites CONSTANT)    // invitations to groups waiting for an answer
    Q_PROPERTY(QObject *members READ members CONSTANT)              // the open group's members: id, name, avatar, color, state, isMe, friend
    Q_PROPERTY(bool currentIsGroup READ currentIsGroup NOTIFY currentChanged)
    Q_PROPERTY(int incomingGroupInvites READ incomingGroupInvites NOTIFY groupInvitesChanged)
    Q_PROPERTY(QObject *profiles READ profiles CONSTANT)            // our own profiles: id, display_name, avatar, default
    Q_PROPERTY(QString myName READ myName NOTIFY profilesChanged)   // the default profile's display name
    Q_PROPERTY(QString myAvatar READ myAvatar NOTIFY profilesChanged)
    Q_PROPERTY(QString current READ current NOTIFY currentChanged)  // the user id of the open chat, "" for none
    Q_PROPERTY(int unreadTotal READ unreadTotal NOTIFY unreadTotalChanged)
    Q_PROPERTY(int incomingRequests READ incomingRequests NOTIFY incomingRequestsChanged)
    Q_PROPERTY(int friendCount READ friendCount NOTIFY peopleCountsChanged)
    Q_PROPERTY(int pendingCount READ pendingCount NOTIFY peopleCountsChanged)
    Q_PROPERTY(QString currentName READ currentName NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentAvatar READ currentAvatar NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentColor READ currentColor NOTIFY currentInfoChanged)
    Q_PROPERTY(QString currentState READ currentState NOTIFY currentInfoChanged)   // stream state of the open chat
    Q_PROPERTY(bool currentCertChanged READ currentCertChanged NOTIFY currentInfoChanged)
    Q_PROPERTY(bool hasOlder READ hasOlder NOTIFY hasOlderChanged)                 // more history than is loaded
    Q_PROPERTY(bool attentive READ attentive WRITE setAttentive NOTIFY attentiveChanged) // someone is looking at the window
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QObject *ponds READ ponds CONSTANT)                  // the Ponds we are in, and the one that is open (ponds.h)
public:
    explicit Chats(QObject *parent = nullptr);

    QObject *people() { return &people_; }
    QObject *conversations() { return &conversations_; }
    QObject *sidebar() { return &sidebar_; }
    QObject *messages() { return &messages_; }
    QObject *blocked() { return &blocked_; }
    QObject *groupInvites() { return &invites_; }
    QObject *members() { return &members_; }
    bool currentIsGroup() const { return currentKind_ == QLatin1String("group"); }
    int incomingGroupInvites() const { return groupInviteCount_; }
    void setSelf(const QString &id) { self_ = id; } // our own user id, so "you" can be said of what we did
    // The Puddle runs in this process: a picked file is handed to it open (puddle_server_take_file: cloned into its data
    // folder, never read again) instead of uploaded through the API. `fn` runs on `pool`; returns 0 with the file's id,
    // -2 if it is too large, -1. Unset (a phone client, or before the Puddle starts): files are uploaded.
    using TakeFile = std::function<int(int fd, char id[65])>;
    void setTakeFile(TakeFile fn, QThreadPool *pool) { takeFile_ = std::move(fn); takePool_ = pool; }
    QObject *profiles() { return &profiles_; }
    QString myName() const { return myName_; }
    QString myAvatar() const { return myAvatar_; }
    QString current() const { return current_; }
    int friendCount() const { return friends_; }
    int pendingCount() const { return pending_; }
    QString currentName() const { return curName_; }
    QString currentAvatar() const { return curAvatar_; }
    QString currentColor() const { return curColor_; }
    QString currentState() const { return curState_; }
    bool currentCertChanged() const { return curCert_; }
    bool hasOlder() const { return hasOlder_; }
    bool attentive() const { return attentive_; }
    void setAttentive(bool a);
    int unreadTotal() const { return unreadTotal_; }
    int incomingRequests() const { return incomingRequests_; }
    bool connected() const { return connected_; }
    ApiClient *api() { return &api_; }
    QObject *ponds() { return &ponds_; }
    Ponds *pondList() { return &ponds_; }

    // Start talking to the Puddle at `base`, reading its token and certificate from the files.
    bool attach(const QUrl &base, const QString &tokenFile, const QString &certFile);
    void setInterval(int ms) { timer_.setInterval(ms); }
    void setPageSize(int n) { pageSize_ = n; } // how many of a list to ask for at a time (for tests; the Puddle caps it at 100 or 200)

    // Fetch everything now (also runs on a timer).
    Q_INVOKABLE void refresh();

    Q_INVOKABLE void open(const QString &id, const QString &kind = QStringLiteral("dm")); // show this chat ("dm" with a person, "group") and mark it read
    Q_INVOKABLE void close();
    Q_INVOKABLE void loadOlder();
    Q_INVOKABLE QString userAt(int row) const { return conversations_.at(row).value("id").toString(); }
    Q_INVOKABLE QString kindAt(int row) const { return conversations_.at(row).value("kind").toString(); }
    Q_INVOKABLE QString sidebarIdAt(int row) const { return sidebar_.at(row).value("id").toString(); }     // for stepping through the list
    Q_INVOKABLE QString sidebarKindAt(int row) const { return sidebar_.at(row).value("kind").toString(); }
    Q_INVOKABLE QVariantMap messageAt(int row) const { return messages_.at(row).toVariantMap(); }
    Q_INVOKABLE void retry();                           // try again to deliver what is stuck in the open chat
    Q_INVOKABLE void send(const QString &text, const QString &replyTo = QString());
    // A message with files, to the open chat: each of `files` ({key, path, name, mime, w?, h?, duration_ms?, preview?}) is
    // uploaded a chunk at a time (uploadProgress), then the message goes with them (filesSent), or not (filesFailed).
    // `text` may be empty.
    Q_INVOKABLE void sendFiles(const QString &text, const QString &replyTo, const QVariantList &files);
    // The same, to another place's send route (`path`, such as a Pond channel's .../messages/send), rather than the open chat.
    void sendFilesTo(const QString &path, const QString &text, const QVariantList &files);
    Q_INVOKABLE void edit(const QString &messageId, const QString &text);
    Q_INVOKABLE void remove(const QString &messageId);

    // groups
    // Friends to pick from, for the "+" dialog. With no text, the most recent ones; with text, those with it in their name, the ones whose name
    // starts with it first, then the rest, each in order of recent activity. At most `limit`. Each is { id, name, avatar, color }.
    Q_INVOKABLE QVariantList findFriends(const QString &text, int limit) const;
    Q_INVOKABLE int friendMatches(const QString &filter) const;
    // What we know of a person we have a stream with, for their profile in the side panel: { name, blurb, about, pronouns, state, since, address, certChanged }.
    // Empty if there is no such stream.
    Q_INVOKABLE QVariantMap person(const QString &userId) const; // how many friends have `filter` (any case) in their name; all of them if empty
    Q_INVOKABLE bool isGroupMember(const QString &userId) const { for (const QJsonObject &m : members_.items()) if (m.value("id").toString() == userId) return true; return false; }
    // `profile`, in these four, is which of the owner's profiles they will see; empty: none (they see only the id)
    Q_INVOKABLE void createGroup(const QString &name, const QStringList &friends, const QString &profile = QString());
    Q_INVOKABLE void inviteToGroup(const QString &userId);   // into the open group
    Q_INVOKABLE void acceptGroup(const QString &groupId, const QString &profile = QString());
    Q_INVOKABLE void declineGroup(const QString &groupId);
    Q_INVOKABLE void leaveGroup();                           // the open one
    Q_INVOKABLE void leaveGroup(const QString &groupId);     // any group we are in (from a menu on its row)
    Q_INVOKABLE void renameGroup(const QString &name);       // the open one
    Q_INVOKABLE void renameGroup(const QString &groupId, const QString &name);

    Q_INVOKABLE void accept(const QString &userId, const QString &profile = QString());
    Q_INVOKABLE void decline(const QString &userId);
    Q_INVOKABLE void cancel(const QString &userId);      // withdraw our own request, or end a stream we no longer want
    Q_INVOKABLE void block(const QString &userId);
    Q_INVOKABLE void unblock(const QString &userId);
    Q_INVOKABLE void trust(const QString &userId);       // accept their changed certificate
    Q_INVOKABLE void request(const QString &link, const QString &note, const QString &profile = QString());

    // ---- files in messages (docs/files.md) ----
    // The file `id` (called `name`) as a file on this computer, to show or play: its URL once it is in the app's cache, else ""
    // and it is fetched from the Puddle now if the Puddle has it whole (fileProgress on the way, then fileReady).
    Q_INVOKABLE QString fileUrl(const QString &id, const QString &name);
    Q_INVOKABLE bool fileFetching(const QString &id) const { return downloading_.contains(id); }
    // Ask the Puddle to fetch a file of a message in the open chat that it did not fetch by itself, or failed to; or to stop.
    Q_INVOKABLE void fetchFile(const QString &messageId, const QString &fileId);
    Q_INVOKABLE void cancelFile(const QString &messageId, const QString &fileId);
    // Ask again for a file whose holders have sent it whole before (docs/files.md, "Asking again").
    Q_INVOKABLE void requestFile(const QString &messageId, const QString &fileId);
    // Someone asked for a file of ours again: send it to them once (yes), or not.
    Q_INVOKABLE void answerFileRequest(const QString &messageId, const QString &fileId, const QString &userId, bool yes);
    // Keep a copy where the owner says (a file: URL from a save dialog). False if it is not here yet or cannot be written.
    Q_INVOKABLE bool saveFile(const QString &id, const QString &name, const QUrl &to);
    // Open it with the system's own app for its kind.
    Q_INVOKABLE bool openFile(const QString &id, const QString &name);
    Q_INVOKABLE static QString sizeText(double bytes);   // "3.2 MB"
    void setFileCacheDir(const QString &dir) { fileCacheDir_ = dir; }

    // The picture with this hash as a file: its URL, or "" while it is being fetched (then avatarReady is emitted).
    Q_INVOKABLE QString avatarFor(const QString &hash);
    void setCacheDir(const QString &dir) { cacheDir_ = dir; }

    // A short code derived from an id, e.g. "K7QM-2XPD", to compare with the person out loud. And the id in groups of four.
    Q_INVOKABLE void copy(const QString &text);
    Q_INVOKABLE static QString shortCode(const QString &userId);
    Q_INVOKABLE static QString grouped(const QString &userId);

    // What the person is called here: their display name, else a short form of the id.
    Q_INVOKABLE QString nameOf(const QString &userId) const;

signals:
    void currentChanged();
    void unreadTotalChanged();
    void incomingRequestsChanged();
    void connectedChanged();
    void failed(const QString &what);                    // something the owner asked for did not work
    void newIncoming(const QString &userId, int unread); // for notifications: a chat has more unread than before
    void newRequest(const QString &userId);
    void groupInvitesChanged();
    void newGroupInvite(const QString &groupId);
    void newFileRequest(const QString &userId, const QString &conversationId); // for notifications: someone asked for a file again
    void groupCreated(const QString &groupId);
    void groupLeft();
    void avatarReady(const QString &hash);
    void uploadProgress(const QString &key, double fraction);
    void fileProgress(const QString &id, double fraction);
    void fileReady(const QString &id);
    void filesSent();
    void filesFailed(const QString &why);
    void profilesChanged();
    void currentInfoChanged();
    void hasOlderChanged();
    void attentiveChanged();
    void peopleCountsChanged();

private:
    void refreshPeople();
    void refreshConversations();
    void refreshMessages();
    void updateCurrentInfo();
    void refreshBlocked();
    void refreshGroups();
    void refreshProfiles();
    void refreshFileRequests();
    void post(const QString &path, const QJsonObject &body, const QString &failure, bool andRefresh = true);
    void setConnected(bool c);
    void markRead();
    bool notice(QJsonObject &n) const;
    void rebuildMembers();
    void rebuildSidebar();
    // GET a list that comes in pages (`next` is the offset of the following one) and hand over all of it, as one document with the same key
    void getAll(const QString &path, const QString &key, std::function<void(int, const QJsonDocument &)> done);

    ApiClient api_;
    Ponds ponds_{ &api_ };
    QTimer timer_;
    JsonListModel people_, conversations_, messages_, blocked_, profiles_, invites_, members_, sidebar_;
    QHash<QString, QJsonObject> groups_;   // the groups we are in or invited to, by id
    QSet<QString> invitedTo_;
    QString currentKind_ = QStringLiteral("dm"), self_;
    int groupInviteCount_ = 0;
    bool groupsLoaded_ = false;
    QString curName_, curAvatar_, curColor_, curState_;
    bool curCert_ = false, hasOlder_ = false, attentive_ = true;
    QString myName_, myAvatar_, cacheDir_, fileCacheDir_;
    QSet<QString> downloading_;
    QString cachedPath(const QString &id, const QString &name) const;
    QSet<QString> fetching_;
    QString current_;
    QHash<QString, QString> names_, avatars_, colors_;
    QHash<QString, int> unread_;
    QSet<QString> requested_;
    bool peopleLoaded_ = false, conversationsLoaded_ = false;
    int friends_ = 0, pending_ = 0;
    int unreadTotal_ = 0, incomingRequests_ = 0;
    // The open chat's history is read a page at a time (newest first; `next` is the offset of the page before) and everything read is kept, by
    // id, so scrolling back never leaves a gap however many messages arrive meanwhile. The server cuts a page short when messages are long.
    QHash<QString, QJsonObject> msgs_;      // every message read so far
    QHash<QString, QJsonObject> notices_;   // and what happened to the group, by event id
    qint64 olderNext_ = -1;                 // the offset of the next page back; -1 when there is none
    bool loadedOlder_ = false, loadingOlder_ = false;
    void renderMessages();
    struct FileSend;
    void uploadFile(std::shared_ptr<FileSend> job, int index);
    void uploadChunk(std::shared_ptr<FileSend> job, int index, const QString &token, int n, int count);
    void sendWithFiles(std::shared_ptr<FileSend> job);
    void takeFile(std::shared_ptr<FileSend> job, int index);
    TakeFile takeFile_;
    QThreadPool *takePool_ = nullptr;
    bool bridging_ = false; // more arrived since the last look than one page holds: reading back until what is new joins up with what is held
    void bridgeGap(const QString &user, std::shared_ptr<QSet<QString>> held, qint64 offset, int pages);
    // A message of ours that is still being delivered, further back than the newest page, is read again (its page is) until the state is fresh.
    void refreshBack(const QString &user, double oldestTs, qint64 offset, int pages);
    static constexpr int kMessagePage = 50;
    int pageSize_ = 100;
    bool connected_ = true; // until a request fails, so the banner does not flash at start-up
    bool marking_ = false, profilesLoaded_ = false;
    bool busy_[7] = { false, false, false, false, false, false, false };
    QSet<QString> fileRequests_;   // "<user> <message> <file>" of each request seen, to notify of new ones
    bool fileRequestsLoaded_ = false;
};
