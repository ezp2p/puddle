#include "chats.h"
#include <QLocale>
#include <QDesktopServices>

#include <QFileInfo>

#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QPainter>
#include <algorithm>
#include <QStandardPaths>
#include <QJsonArray>
#include <QRegularExpression>
#include <QUrlQuery>

namespace {

QString shortId(const QString &id)
{
    return id.left(8);
}

// The owner API names what it acts on in the path: "/api/v1/<kind>/<id>/<rest>".
QString path(const QString &kind, const QString &id, const QString &rest = QString())
{
    return QStringLiteral("/api/v1/%1/%2").arg(kind, id) + (rest.isEmpty() ? QString() : QStringLiteral("/") + rest);
}

QString idOf(const QJsonValue &person) { return person.toObject().value("id").toString(); }

// A person as an answer wrote them (a PartialUser): as they show themselves where it was written, so a group's members and
// messages show each person as they are in that group, not as they show themselves to us directly.
QString nameIn(const QJsonValue &person) { return person.toObject().value("display_name").toString(); }
QString avatarIn(const QJsonValue &person) { return person.toObject().value("avatar").toObject().value("hash").toString(); }

// A page of a conversation holds its messages and what happened to the group, in one list told apart by `type`. The window keeps the two
// apart and in the shape it reads: who said it as `sender`, and an `edited` flag beside `edited_at`.
QJsonObject asMessage(QJsonObject m)
{
    m.insert("sender", idOf(m.value("author")));
    if (m.contains("edited_at"))
        m.insert("edited", true);
    return m;
}

QJsonObject asNotice(QJsonObject n)
{
    n.insert("sender", idOf(n.value("author")));
    n.insert("type", n.value("type").toString().mid(6)); // "group_join" -> "join"
    if (n.contains("member"))
        n.insert("member", idOf(n.value("member")));
    return n;
}

QList<QJsonObject> messagesOf(const QJsonDocument &doc)
{
    QList<QJsonObject> out;
    for (const QJsonValue &v : doc.object().value("items").toArray())
        if (v.toObject().value("type").toString() == QLatin1String("default"))
            out.append(asMessage(v.toObject()));
    return out;
}

QList<QJsonObject> noticesOf(const QJsonDocument &doc)
{
    QList<QJsonObject> out;
    for (const QJsonValue &v : doc.object().value("items").toArray())
        if (v.toObject().value("type").toString().startsWith(QLatin1String("group_")))
            out.append(asNotice(v.toObject()));
    return out;
}

} // namespace

Chats::Chats(QObject *parent)
    : QObject(parent),
      people_({ "id", "name", "state", "note", "error", "avatar", "color", "updated_at", "address", "provisional" }),
      conversations_({ "id", "kind", "user", "group", "name", "avatar", "color", "stream", "last", "lastBy", "unread", "members" }, nullptr, "id"),
      messages_({ "id", "sender", "senderName", "ts", "body", "files", "outgoing", "edited", "deleted", "reply_to", "avatar", "tint", "grouped",
                  "dayStart", "replyName", "replyText", "replyMissing", "deliveryState", "deliveryReason", "deliveryNext", "kind", "noticeText" }),
      blocked_({ "id", "user", "name", "avatar", "created_at" }, nullptr, "user"),
      dmBlocks_({ "id", "user", "name", "avatar", "created_at" }, nullptr, "user"),
      profiles_({ "id", "display_name", "avatar", "default", "pronouns", "blurb", "place_count" }),
      invites_({ "id", "name", "invitedBy", "invitedByName" }),
      members_({ "id", "name", "avatar", "color", "state", "isMe", "friend" }),
      sidebar_({ "key", "kind", "id", "name", "avatar", "color", "preview", "lastTs", "unread", "state", "members", "note", "invitedBy", "section",
                 "pond", "pondName", "pondIcon" }, nullptr, "key"),
      devices_({ "id", "name", "state", "check", "created_at", "last_seen" }, nullptr, "id")
{
    cacheDir_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/avatars";
    fileCacheDir_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/files";
    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, &Chats::refresh);
    missingTimer_.setInterval(3000);
    connect(&missingTimer_, &QTimer::timeout, this, &Chats::askAgain);
    calls_.setNameOf([this](const QString &u) { return nameOf(u); });
    connect(&calls_, &Calls::historyChanged, this, &Chats::refreshCallNotes);
    connect(&ponds_, &Ponds::listChanged, this, [this] { // a Pond's icon is on the pictures of those met through it
        rebuildSidebar();
        if (currentIsPondDm())
            emit currentInfoChanged();
    });
}

bool Chats::attach(const QUrl &base, const QString &tokenFile, const QString &certFile)
{
    QFile f(tokenFile);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray token = f.readAll().trimmed();
    if (!api_.configure(base, token, certFile))
        return false;
    timer_.start();
    refresh();
    ponds_.start();
    calls_.start();
    return true;
}

void Chats::setConnected(bool c)
{
    if (connected_ == c)
        return;
    connected_ = c;
    emit connectedChanged();
}

void Chats::refresh()
{
    refreshPeople();
    refreshConversations();
    refreshMessages();
    refreshBlocked();
    refreshGroups();
    refreshFileRequests();
    /* the profiles too, now and then: they can be changed from another device (a phone, the app on another computer) */
    if (!profilesLoaded_ || ++profileTicks_ % 5 == 0)
        refreshProfiles();
    if (!api_.isDevice()) {
        refreshDevices();
        refreshLevee();
    }
    if (pairingLink_.size() && pairingExpires_ <= QDateTime::currentMSecsSinceEpoch()) {
        pairingLink_.clear(); /* it has run out: nobody can use it, so it is not shown */
        emit pairingChanged();
    }
}

bool Chats::attachDevice(const QUrl &base, const QString &puddleId, const QByteArray &puddleKey, const QByteArray &deviceSeed)
{
    if (!api_.configureDevice(base, puddleId, puddleKey, deviceSeed))
        return false;
    setSelf(puddleId);
    timer_.start();
    refresh();
    ponds_.start(); /* through its Puddle, like everything else */
    calls_.setDevice(ApiClient::deviceIdOf(deviceSeed), base.host()); /* its calls have a leg of their own to the Puddle */
    calls_.start();
    return true;
}

// ---- phones (the desktop app's side) ----

void Chats::refreshDevices()
{
    if (busy_[7])
        return;
    busy_[7] = true;
    api_.get(QStringLiteral("/api/v1/devices"), [this](int status, const QJsonDocument &doc) {
        busy_[7] = false;
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        int pending = 0;
        QSet<QString> seen;
        for (const QJsonValue &v : doc.object().value("devices").toArray()) {
            const QJsonObject d = v.toObject();
            rows.append(d);
            const QString id = d.value("id").toString();
            seen.insert(id);
            if (d.value("state").toString() == QLatin1String("pending")) {
                pending++;
                if (devicesLoaded_ && !knownDevices_.contains(id))
                    emit newDevice(d.value("name").toString());
            }
        }
        knownDevices_ = seen;
        devicesLoaded_ = true;
        devices_.setItems(rows);
        if (pending != pendingDevices_) {
            pendingDevices_ = pending;
            emit devicesChanged();
        }
        if (pending && pairingLink_.size()) { /* the link has been used: no need to keep showing it */
            pairingLink_.clear();
            emit pairingChanged();
        }
    });
}

void Chats::startPairing()
{
    api_.post(QStringLiteral("/api/v1/devices/pair"), QJsonObject{}, [this](int status, const QJsonDocument &doc) {
        if (status == 409) {
            emit failed(tr("Your Puddle has no address to give a phone. Set \"advertise:\" in puddle.yaml to the address it can be reached at."));
            return;
        }
        if (status != 201 || !doc.isObject()) {
            emit failed(tr("Could not make a pairing link."));
            return;
        }
        pairingLink_ = doc.object().value("link").toString();
        pairingExpires_ = qint64(doc.object().value("expires_at").toDouble());
        emit pairingChanged();
    });
}

void Chats::stopPairing()
{
    pairingLink_.clear();
    emit pairingChanged();
}

// ---- the Levee (docs/levee.md) ----

void Chats::refreshLevee()
{
    if (leveeLoading_)
        return;
    leveeLoading_ = true;
    api_.get(QStringLiteral("/api/v1/levee"), [this](int status, const QJsonDocument &doc) {
        leveeLoading_ = false;
        if (status != 200 || !doc.isObject() || doc.object() == levee_)
            return;
        levee_ = doc.object();
        emit leveeChanged();
    });
}

void Chats::joinLevee(const QString &link)
{
    if (leveeBusy_)
        return;
    leveeBusy_ = true;
    leveeError_.clear();
    emit leveeChanged();
    api_.post(QStringLiteral("/api/v1/levee/join"), QJsonObject{ { "link", link.trimmed() } }, [this](int status, const QJsonDocument &doc) {
        leveeBusy_ = false;
        if (status == 200 && doc.isObject()) {
            levee_ = doc.object();
        } else {
            const QJsonObject o = doc.object();
            leveeError_ = o.value("error").toString() == QLatin1String("bad_link")
                              ? tr("That isn't a Levee's join link. On the Levee, run: leveectl join")
                              : o.value("message").toString().isEmpty() ? tr("The Levee couldn't be joined.") : o.value("message").toString();
        }
        emit leveeChanged();
    });
}

void Chats::leaveLevee()
{
    if (leveeBusy_)
        return;
    leveeBusy_ = true;
    emit leveeChanged();
    api_.post(QStringLiteral("/api/v1/levee/leave"), QJsonObject{}, [this](int status, const QJsonDocument &) {
        leveeBusy_ = false;
        if (status == 204)
            levee_ = QJsonObject{ { "joined", false } };
        else
            emit failed(tr("Could not leave the Levee."));
        emit leveeChanged();
    });
}

void Chats::setLeveePondVoiceDirect(bool direct)
{
    if (leveeBusy_)
        return;
    leveeBusy_ = true;
    emit leveeChanged();
    api_.post(QStringLiteral("/api/v1/levee/settings"), QJsonObject{ { "pond_voice_direct", direct } }, [this](int status, const QJsonDocument &doc) {
        leveeBusy_ = false;
        if (status == 200 && doc.isObject())
            levee_ = doc.object();
        else
            emit failed(tr("Could not change how Pond voice connects."));
        emit leveeChanged(); /* either way: the switch shows what is so */
    });
}

void Chats::acceptDevice(const QString &id) { post(path("devices", id, "accept"), QJsonObject{}, tr("Could not let that phone in.")); }
void Chats::declineDevice(const QString &id) { post(path("devices", id, "decline"), QJsonObject{}, tr("Could not turn that phone away.")); }
void Chats::revokeDevice(const QString &id) { post(path("devices", id, "revoke"), QJsonObject{}, tr("Could not remove that phone.")); }

void Chats::refreshBlocked()
{
    if (busy_[3])
        return;
    busy_[3] = true;
    api_.get(QStringLiteral("/api/v1/blocks?limit=200"), [this](int status, const QJsonDocument &doc) {
        busy_[3] = false;
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        for (const QJsonValue &v : doc.object().value("items").toArray()) {
            QJsonObject b = v.toObject();
            const QJsonObject who = b.value("user").toObject();
            const QString user = who.value("id").toString();
            b.insert("id", user);
            b.insert("user", user);
            b.insert("name", names_.contains(user) ? nameOf(user) : who.value("display_name").toString());
            b.insert("avatar", names_.contains(user) ? avatars_.value(user) : avatarIn(who));
            rows.append(b);
        }
        blocked_.setItems(rows);
        QSet<QString> ids;
        for (const QJsonObject &b : rows)
            ids.insert(b.value("user").toString());
        ponds_.setBlocked(ids); /* what they write in Ponds isn't shown either */
        emit blockedChanged();
    });
    api_.get(QStringLiteral("/api/v1/dm-blocks"), [this](int status, const QJsonDocument &doc) {
        if (status != 200)
            return;
        QSet<QString> ids;
        QList<QJsonObject> rows;
        for (const QJsonValue &v : doc.object().value("items").toArray()) {
            const QJsonObject who = v.toObject().value("user").toObject();
            const QString user = who.value("id").toString();
            ids.insert(user);
            // as we know them: a friend by their name for us, someone met only in a Pond by what they show there
            const QString shown = who.value("display_name").toString();
            rows.append({ { "id", user }, { "user", user }, { "created_at", v.toObject().value("created_at") },
                          { "name", names_.contains(user) ? nameOf(user) : !shown.isEmpty() ? shown : shortId(user) },
                          { "avatar", names_.contains(user) ? avatars_.value(user) : avatarIn(who) } });
        }
        dmBlocks_.setItems(rows);
        if (ids != dmBlocked_) {
            dmBlocked_ = ids;
            emit blockedChanged();
        }
    });
}

void Chats::refreshProfiles()
{
    if (busy_[4])
        return;
    busy_[4] = true;
    /* where others find us: a phone has no Puddle of its own to ask, so it asks this one */
    api_.get(QStringLiteral("/api/v1/identity"), [this](int status, const QJsonDocument &doc) {
        if (status != 200 || !doc.isObject())
            return;
        const QString link = doc.object().value("link").toString();
        if (link != myLink_) {
            myLink_ = link;
            emit profilesChanged();
        }
    });
    api_.get(QStringLiteral("/api/v1/profiles"), [this](int status, const QJsonDocument &doc) {
        busy_[4] = false;
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        const QJsonArray items = doc.object().value("items").toArray();
        for (const QJsonValue &v : items) {
            QJsonObject p = v.toObject();
            QJsonArray tooLarge; /* pictures over the 1 MiB a Pond keeps: left out of every Pond until chosen again (smaller) */
            for (const char *slot : { "avatar", "banner", "background" })
                if (p.value(slot).toObject().value("size").toDouble() > 1048576)
                    tooLarge.append(QString::fromLatin1(slot));
            p.insert("too_large_for_ponds", tooLarge);
            p.insert("avatar", p.value("avatar").toObject().value("hash").toString());
            p.insert("default", rows.isEmpty()); // the first is how the owner sees themselves
            p.insert("place_count", p.value("places").toArray().size());
            if (rows.isEmpty()) {
                myName_ = p.value("display_name").toString();
                myAvatar_ = p.value("avatar").toString();
                myProfile_ = v.toObject();
                places_.clear();
                placeProfiles_.clear();
            }
            for (const QJsonValue &place : p.value("places").toArray()) {
                places_.insert(place.toString(), { p.value("display_name").toString(), p.value("avatar").toString() });
                placeProfiles_.insert(place.toString(), v.toObject());
            }
            rows.append(p);
        }
        /* the same as last time: nothing to redraw */
        QJsonArray seen;
        for (const QJsonObject &r : rows)
            seen.append(r);
        const QByteArray now = QJsonDocument(seen).toJson(QJsonDocument::Compact);
        if (profilesLoaded_ && now == profilesSeen_)
            return;
        profilesSeen_ = now;
        profilesLoaded_ = true;
        profiles_.setItems(rows);
        emit profilesChanged();
        // the owner's own messages and member row carry their name: show the one just loaded
        if (!current_.isEmpty()) {
            renderMessages();
            if (currentIsGroup())
                rebuildMembers();
        }
    });
}

// ---- files in messages ----

static bool isFileId(const QString &id)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    return hex.match(id).hasMatch();
}

// <cache>/files/<id>.<ext>: named by its id, with the extension its name has, so a player can tell what it is
QString Chats::cachedPath(const QString &id, const QString &name) const
{
    QString ext = QFileInfo(name).suffix().toLower();
    static const QRegularExpression ok(QStringLiteral("^[a-z0-9]{1,8}$"));
    return fileCacheDir_ + "/" + id + (ok.match(ext).hasMatch() ? "." + ext : QString());
}

QString Chats::fileUrl(const QString &id, const QString &name)
{
    if (!isFileId(id))
        return QString();
    const QString path = cachedPath(id, name);
    if (QFile::exists(path))
        return QUrl::fromLocalFile(path).toString();
    if (downloading_.contains(id))
        return QString();
    downloading_.insert(id);
    QDir().mkpath(fileCacheDir_);
    api_.download(QStringLiteral("/api/v1/files/%1/content").arg(id), path,
                  [this, id](qint64 got, qint64 total) { if (total > 0) emit fileProgress(id, double(got) / double(total)); },
                  [this, id](bool ok) {
                      downloading_.remove(id);
                      if (ok)
                          emit fileReady(id);
                  });
    return QString();
}

void Chats::fetchFile(const QString &messageId, const QString &fileId)
{
    if (current_.isEmpty() || !isFileId(fileId))
        return;
    post(path("conversations", current_, QStringLiteral("messages/%1/files/%2/download").arg(messageId, fileId)), QJsonObject{},
         tr("The file could not be fetched."));
}

void Chats::requestFile(const QString &messageId, const QString &fileId)
{
    if (current_.isEmpty() || !isFileId(fileId))
        return;
    post(path("conversations", current_, QStringLiteral("messages/%1/files/%2/request").arg(messageId, fileId)), QJsonObject{},
         tr("The file could not be asked for."));
}

void Chats::answerFileRequest(const QString &messageId, const QString &fileId, const QString &userId, bool yes)
{
    if (current_.isEmpty() || !isFileId(fileId) || userId.isEmpty())
        return;
    post(path("conversations", current_, QStringLiteral("messages/%1/files/%2/requests/%3/%4").arg(messageId, fileId, userId, yes ? "accept" : "decline")),
         QJsonObject{}, yes ? tr("The file could not be sent.") : tr("The request could not be declined."));
}

// Requests for files we sent, waiting for the owner: shown on their messages, and a notification for each new one.
void Chats::refreshFileRequests()
{
    if (busy_[6])
        return;
    busy_[6] = true;
    api_.get(QStringLiteral("/api/v1/files/requests"), [this](int status, const QJsonDocument &doc) {
        busy_[6] = false;
        if (status != 200 || !doc.isObject())
            return;
        QSet<QString> seen;
        for (const QJsonValue &v : doc.object().value("requests").toArray()) {
            const QJsonObject r = v.toObject();
            const QString key = r.value("user").toString() + ' ' + r.value("message").toString() + ' ' + r.value("file").toString();
            seen.insert(key);
            if (fileRequestsLoaded_ && !fileRequests_.contains(key))
                emit newFileRequest(r.value("user").toString(), r.value("conversation").toString());
        }
        fileRequests_ = seen;
        fileRequestsLoaded_ = true;
    });
}

void Chats::cancelFile(const QString &messageId, const QString &fileId)
{
    if (current_.isEmpty() || !isFileId(fileId))
        return;
    post(path("conversations", current_, QStringLiteral("messages/%1/files/%2/cancel").arg(messageId, fileId)), QJsonObject{},
         tr("The download could not be stopped."));
}

bool Chats::saveFile(const QString &id, const QString &name, const QUrl &to)
{
    const QString from = cachedPath(id, name), dest = to.toLocalFile();
    if (!QFile::exists(from) || dest.isEmpty())
        return false;
    QFile::remove(dest);
    if (!QFile::copy(from, dest)) {
        emit failed(tr("%1 could not be saved there.").arg(name));
        return false;
    }
    return true;
}

bool Chats::openFile(const QString &id, const QString &name)
{
    const QString from = cachedPath(id, name);
    if (!QFile::exists(from))
        return false;
    // a copy under its own name, so the system's app shows that name and knows the kind of file
    QString safe = QFileInfo(name).fileName();
    safe.remove(QRegularExpression(QStringLiteral("[\\x00-\\x1f/\\\\:]")));
    if (safe.isEmpty() || safe.startsWith('.'))
        safe = "file" + safe;
    const QString dir = fileCacheDir_ + "/open/" + id.left(16), to = dir + "/" + safe;
    QDir().mkpath(dir);
    if (!QFile::exists(to) && !QFile::copy(from, to))
        return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(to));
}

QString Chats::sizeText(double bytes)
{
    return QLocale().formattedDataSize(qint64(bytes), 1, QLocale::DataSizeSIFormat);
}

QString Chats::avatarFor(const QString &hash)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    if (!hex.match(hash).hasMatch())
        return QString();
    const QString path = cacheDir_ + "/" + hash;
    if (QFile::exists(path))
        return QUrl::fromLocalFile(path).toString();
    if (fetching_.contains(hash))
        return QString();
    fetching_.insert(hash);
    api_.getBytes(QStringLiteral("/api/v1/media?hash=") + hash, [this, hash, path](int status, const QByteArray &bytes) {
        fetching_.remove(hash);
        if (status == 404 || status == 0) { // not on our Puddle yet (it fetches a friend's or a Pond's picture by itself): ask again
            if (!missing_.contains(hash))
                missing_.insert(hash, QDateTime::currentMSecsSinceEpoch());
            if (!missingTimer_.isActive())
                missingTimer_.start();
            return;
        }
        missing_.remove(hash);
        if (status != 200 || bytes.isEmpty() || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != hash.toLatin1())
            return; // not what was asked for: not shown
        QDir().mkpath(cacheDir_);
        QFile f(path + ".part");
        if (f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size()) {
            f.close();
            QFile::remove(path);
            f.rename(path);
            emit avatarReady(hash);
        }
    });
    return QString();
}

// Every few seconds for the first two minutes, then once a minute: a friend who is away sends their picture when they are back.
void Chats::askAgain()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const bool slow = now - missingSlow_ >= 60000;
    if (slow)
        missingSlow_ = now;
    for (auto it = missing_.cbegin(); it != missing_.cend(); ++it)
        if (now - it.value() < 120000 || slow)
            avatarFor(it.key());
    if (missing_.isEmpty())
        missingTimer_.stop();
}

void Chats::copy(const QString &text)
{
    if (QClipboard *c = QGuiApplication::clipboard())
        c->setText(text);
}

QString Chats::grouped(const QString &id)
{
    QStringList parts;
    for (qsizetype i = 0; i < id.size(); i += 4)
        parts << id.mid(i, 4);
    return parts.join(' ');
}

QString Chats::shortCode(const QString &id)
{
    static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // no I, O, 0, 1
    const QByteArray h = QCryptographicHash::hash(QByteArray("puddle-code\n") + id.toUtf8(), QCryptographicHash::Sha256);
    QString out;
    for (int i = 0; i < 8; i++) {
        if (i == 4)
            out += '-';
        out += QLatin1Char(alphabet[uchar(h[i]) % 32]);
    }
    return out;
}

void Chats::getAll(const QString &path, const QString &key, std::function<void(int, const QJsonDocument &)> done)
{
    auto all = std::make_shared<QJsonArray>();
    auto step = std::make_shared<std::function<void(int)>>();
    *step = [this, path, key, done, all, step](int offset) {
        api_.get(path + QStringLiteral("&offset=%1").arg(offset), [key, done, all, step](int status, const QJsonDocument &doc) {
            const QJsonValue next = doc.object().value("next");
            if (status == 200 && doc.isObject()) {
                for (const QJsonValue &v : doc.object().value("items").toArray())
                    all->append(v);
                if (next.isDouble() && all->size() < 20000) {
                    (*step)(int(next.toDouble()));
                    return;
                }
                done(200, QJsonDocument(QJsonObject{ { key, *all } }));
            } else {
                done(status, doc);
            }
            *step = nullptr; // the function holds itself; let go of it now it is done
        });
    };
    (*step)(0);
}

QVariantList Chats::findFriends(const QString &text, int limit) const
{
    const QString q = text.trimmed();
    QVariantList starts, contains;
    for (const QJsonObject &row : sidebar_.items()) { // already newest first
        if (row.value("kind").toString() != QLatin1String("dm") || row.value("state").toString() != QLatin1String("active"))
            continue; // not a group, nor someone whose connection is closed
        const QString name = row.value("name").toString();
        const bool prefix = q.isEmpty() || name.startsWith(q, Qt::CaseInsensitive);
        if (!prefix && !name.contains(q, Qt::CaseInsensitive))
            continue;
        QVariantMap f{ { "id", row.value("id").toString() }, { "name", name }, { "avatar", row.value("avatar").toString() }, { "color", row.value("color").toString() } };
        (prefix ? starts : contains).append(f);
        if (starts.size() >= limit)
            break;
    }
    QVariantList out = starts;
    for (const QVariant &v : contains)
        if (out.size() < limit)
            out.append(v);
    return out;
}

QVariantMap Chats::person(const QString &userId) const
{
    for (const QJsonObject &p : people_.items()) {
        if (p.value("id").toString() != userId)
            continue;
        const QJsonObject profile = p.value("user").toObject().value("profile").toObject();
        return QVariantMap{ { "name", p.value("name").toString() },
                            { "blurb", profile.value("blurb").toString() },
                            { "about", profile.value("about").toString() },
                            { "pronouns", profile.value("pronouns").toString() },
                            { "state", p.value("state").toString() },
                            { "since", qint64(p.value("created_at").toDouble()) },
                            { "address", p.value("address").toString() } };
    }
    return {};
}

QVariantMap Chats::card(const QString &userId) const
{
    const bool me = !self_.isEmpty() && userId == self_;
    bool isFriend = false;
    for (const QJsonObject &p : people_.items())
        if (p.value("id").toString() == userId)
            isFriend = p.value("state").toString() == QLatin1String("active");
    QVariantMap c;
    for (const QJsonObject &m : members_.items()) /* as they are in the open group */
        if (m.value("id").toString() == userId)
            c = { { "name", m.value("name").toString() }, { "avatar", m.value("avatar").toString() } };
    if (c.isEmpty())
        c = me ? meIn(currentPlace()) : QVariantMap{ { "name", nameOf(userId) }, { "avatar", avatars_.value(userId) } };
    if (!me) { /* ours came whole from meIn; theirs is filled in by loadCard */
        const QString color = colors_.value(userId);
        c.insert("colors", color.isEmpty() ? QVariantList() : QVariantList{ color });
    }
    c.insert("id", userId);
    c.insert("isMe", me);
    c.insert("friend", isFriend);
    c.insert("place", currentIsGroup() ? currentName() : QString());
    if (currentIsPondDm() && !me) { // met through a Pond: its icon on their picture
        const QVariantMap pdm = currentPondDm();
        c.insert("pond", pdm.value("pond"));
        c.insert("pondName", pdm.value("pondName"));
        c.insert("pondIcon", pdm.value("pondIcon"));
    }
    return c;
}

void Chats::loadCard(const QString &userId)
{
    if (userId.isEmpty() || (!self_.isEmpty() && userId == self_))
        return;
    const QString place = currentIsGroup() ? QStringLiteral("group:") + current_ : QStringLiteral("dm");
    api_.get(QStringLiteral("/api/v1/users/%1/profiles").arg(userId), [this, userId, place](int status, const QJsonDocument &doc) {
        if (status != 200)
            return;
        QJsonObject chosen;
        for (const QJsonValue &v : doc.object().value("items").toArray()) {
            const QJsonObject p = v.toObject();
            if (p.value("place").toString() == place || (chosen.isEmpty() && p.value("place").toString() == QLatin1String("dm")))
                chosen = p;
        }
        if (chosen.isEmpty())
            return; /* they show us nothing here: the card keeps what it has */
        QVariantMap c = Ponds::cardOf(chosen);
        c.insert("id", userId);
        emit cardLoaded(userId, c);
    });
}

int Chats::friendMatches(const QString &filter) const
{
    int n = 0;
    for (const QJsonObject &p : people_.items())
        if (p.value("state").toString() == QLatin1String("active") && (filter.isEmpty() || p.value("name").toString().contains(filter, Qt::CaseInsensitive)))
            n++;
    return n;
}

void Chats::refreshPeople()
{
    if (busy_[0])
        return;
    busy_[0] = true;
    getAll(QStringLiteral("/api/v1/streams?limit=%1").arg(qMin(pageSize_, 200)), QStringLiteral("streams"), [this](int status, const QJsonDocument &doc) {
        busy_[0] = false;
        setConnected(status != 0);
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        int incoming = 0, friends = 0, pending = 0;
        QHash<QString, QString> names, avatars, colors;
        QSet<QString> seen;
        for (const QJsonValue &v : doc.object().value("streams").toArray()) {
            QJsonObject s = v.toObject();
            const QJsonObject profile = s.value("user").toObject().value("profile").toObject();
            const QString id = s.value("id").toString();
            QString name = profile.value("display_name").toString();
            if (name.isEmpty() && s.value("provisional").toBool())
                name = s.value("address").toString(); // nobody has answered yet: all we know is where we asked
            if (name.isEmpty())
                name = shortId(id);
            names[id] = name;
            avatars[id] = profile.value("avatar").toObject().value("hash").toString();
            colors[id] = Ponds::cardOf(profile).value("colors").toList().value(0).toString(); /* one colour, or a gradient's first */
            s.insert("name", name);
            s.insert("avatar", profile.value("avatar").toObject().value("hash").toString());
            s.insert("color", colors[id]);
            s.insert("note", s.value("message"));
            const QString state = s.value("state").toString();
            if (state == "active")
                friends++;
            else if (state == "pending_in" || state == "pending_out")
                pending++;
            if (state == "pending_in") {
                incoming++;
                seen.insert(id);
                if (peopleLoaded_ && !requested_.contains(id))
                    emit newRequest(id);
            }
            rows.append(s);
        }
        requested_ = seen;
        peopleLoaded_ = true;
        names_ = names;
        avatars_ = avatars;
        colors_ = colors;
        people_.setItems(rows);
        rebuildSidebar();
        updateCurrentInfo();
        if (friends != friends_ || pending != pending_) {
            friends_ = friends;
            pending_ = pending;
            emit peopleCountsChanged();
        }
        if (incoming != incomingRequests_) {
            incomingRequests_ = incoming;
            emit incomingRequestsChanged();
        }
    });
}

void Chats::refreshConversations()
{
    if (busy_[1])
        return;
    busy_[1] = true;
    getAll(QStringLiteral("/api/v1/conversations?limit=%1").arg(qMin(pageSize_, 100)), QStringLiteral("conversations"), [this](int status, const QJsonDocument &doc) {
        busy_[1] = false;
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        int total = 0;
        QHash<QString, int> unread;
        for (const QJsonValue &v : doc.object().value("conversations").toArray()) {
            QJsonObject c = v.toObject();
            const bool isGroup = c.value("kind").toString() == QLatin1String("group");
            const bool isPondDm = c.value("kind").toString() == QLatin1String("pond_dm");
            const QString user = c.value("id").toString(); // the id of the chat: who, or which group
            c.insert("last", c.value("last_message"));
            if (isPondDm) { // "Ann @ Lily Pad": never mistaken for a friend
                const QJsonObject who = c.value("recipient").toObject();
                const QString name = who.value("display_name").toString() + QStringLiteral(" @ ") + c.value("pond").toObject().value("name").toString();
                c.insert("user", who.value("id"));
                c.insert("name", name);
                c.insert("avatar", who.value("avatar").toObject().value("hash"));
                c.insert("color", who.value("color").isString() ? who.value("color") : QJsonValue(QString()));
                names_.insert(user, name);
                pondDms_.insert(user, c);
            } else if (isGroup) {
                names_.insert(user, c.value("name").toString()); // so a notification can name it
                c.insert("members", c.value("group").toObject().value("member_count"));
                c.insert("group", user);
                const QJsonObject last = c.value("last").toObject();
                if (!last.isEmpty()) // who said it, for the preview line
                    c.insert("lastBy", last.value("outgoing").toBool() ? tr("You") : nameOf(idOf(last.value("author"))));
            } else {
                c.insert("user", user);
                c.insert("name", nameOf(user));
                c.insert("avatar", avatars_.value(user));
                c.insert("color", colors_.value(user));
            }
            int n = c.value("unread").toInt();
            if (user == current_ && attentive_) {
                if (n > 0)
                    markRead(); // being looked at
                n = 0;
                c.insert("unread", 0);
            }
            unread[user] = n;
            if (conversationsLoaded_ && n > unread_.value(user))
                emit newIncoming(user, n);
            total += n;
            rows.append(c);
        }
        unread_ = unread;
        conversationsLoaded_ = true;
        conversations_.setItems(rows);
        rebuildSidebar();
        if (total != unreadTotal_) {
            unreadTotal_ = total;
            emit unreadTotalChanged();
        }
    });
}

void Chats::markRead()
{
    if (current_.isEmpty() || marking_)
        return;
    marking_ = true;
    api_.post(path("conversations", current_, "read"), QJsonObject{}, [this](int, const QJsonDocument &) { marking_ = false; });
}

// Take a page from the server: its messages and notices are kept, and the offset of the page before it remembered when it is the furthest back.
void Chats::refreshMessages()
{
    if (current_.isEmpty() || busy_[2] || bridging_)
        return;
    busy_[2] = true;
    const QString user = current_;
    const QString url = path("conversations", user, QStringLiteral("messages?limit=%1").arg(kMessagePage));
    api_.get(url, [this, user](int status, const QJsonDocument &doc) {
        busy_[2] = false;
        if (status != 200 || !doc.isObject() || user != current_)
            return;
        // The newest page joins what is held if it shares a message with it. If not, more than a page arrived since the last look (a burst), and
        // there is a hole between: it is closed by reading back from this page until a message already held turns up.
        bool joins = msgs_.isEmpty();
        const QList<QJsonObject> page = messagesOf(doc);
        for (const QJsonObject &m : page)
            joins = joins || msgs_.contains(m.value("id").toString());
        std::shared_ptr<QSet<QString>> held;
        if (!joins)
            held = std::make_shared<QSet<QString>>(QSet<QString>(msgs_.keyBegin(), msgs_.keyEnd()));
        for (const QJsonObject &m : page)
            msgs_.insert(m.value("id").toString(), m); // the newest page again: edits, deletions and delivery states are fresh
        for (const QJsonObject &n : noticesOf(doc))
            notices_.insert(n.value("id").toString(), n);
        const QJsonValue next = doc.object().value("next");
        if (held && !next.isNull()) {
            bridging_ = true;
            renderMessages();
            bridgeGap(user, held, qint64(next.toDouble()), 0);
            return;
        }
        if (!loadedOlder_)
            olderNext_ = next.isNull() ? -1 : qint64(next.toDouble());
        // what we sent that is still being delivered, but is older than this page, has a state we only saw once: look at it again
        QSet<QString> live;
        for (const QJsonObject &m : page)
            live.insert(m.value("id").toString());
        double oldestPending = 0;
        for (const QJsonObject &m : msgs_)
            if (m.contains("delivery") && !live.contains(m.value("id").toString()) && (oldestPending == 0 || m.value("ts").toDouble() < oldestPending))
                oldestPending = m.value("ts").toDouble();
        if (oldestPending > 0 && !next.isNull()) {
            bridging_ = true;
            renderMessages();
            refreshBack(user, oldestPending, qint64(next.toDouble()), 0);
            return;
        }
        renderMessages();
    });
}

void Chats::refreshBack(const QString &user, double oldestTs, qint64 offset, int pages)
{
    const QString url = path("conversations", user, QStringLiteral("messages?limit=%1&offset=%2").arg(kMessagePage).arg(offset));
    api_.get(url, [this, user, oldestTs, pages](int status, const QJsonDocument &doc) {
        if (status != 200 || !doc.isObject() || user != current_) {
            bridging_ = false;
            return;
        }
        double oldest = 1e300;
        for (const QJsonObject &m : messagesOf(doc)) {
            msgs_.insert(m.value("id").toString(), m); // the fresh copy replaces the old one
            oldest = qMin(oldest, m.value("ts").toDouble());
        }
        const QJsonValue next = doc.object().value("next");
        if (oldest > oldestTs && !next.isNull() && pages < 100) {
            refreshBack(user, oldestTs, qint64(next.toDouble()), pages + 1);
            return;
        }
        bridging_ = false;
        renderMessages();
    });
}

// Read back from `offset` until a page holds a message that was held before (or the chat starts), so nothing is missing from the middle.
void Chats::bridgeGap(const QString &user, std::shared_ptr<QSet<QString>> held, qint64 offset, int pages)
{
    const QString url = path("conversations", user, QStringLiteral("messages?limit=%1&offset=%2").arg(kMessagePage).arg(offset));
    api_.get(url, [this, user, held, pages](int status, const QJsonDocument &doc) {
        if (status != 200 || !doc.isObject() || user != current_) {
            bridging_ = false;
            return;
        }
        bool joined = false;
        for (const QJsonObject &m : messagesOf(doc)) {
            joined = joined || held->contains(m.value("id").toString());
            msgs_.insert(m.value("id").toString(), m);
        }
        for (const QJsonObject &n : noticesOf(doc))
            notices_.insert(n.value("id").toString(), n);
        const QJsonValue next = doc.object().value("next");
        if (!joined && !next.isNull() && pages < 400) {
            bridgeGap(user, held, qint64(next.toDouble()), pages + 1);
            return;
        }
        if (!joined && !next.isNull()) {                    // an enormous burst: give up what was held before rather than show a hole
            for (const QString &id : *held)
                msgs_.remove(id);
        }
        if (!loadedOlder_ || !joined)                       // where the page before the oldest held one starts
            olderNext_ = next.isNull() ? -1 : qint64(next.toDouble());
        if (!joined)
            loadedOlder_ = false;
        bridging_ = false;
        renderMessages();
    });
}

// One page further back.
void Chats::loadOlder()
{
    if (current_.isEmpty() || loadingOlder_ || olderNext_ < 0)
        return;
    loadingOlder_ = true;
    const QString user = current_;
    const QString url = path("conversations", user, QStringLiteral("messages?limit=%1&offset=%2").arg(kMessagePage).arg(olderNext_));
    api_.get(url, [this, user](int status, const QJsonDocument &doc) {
        loadingOlder_ = false;
        if (status != 200 || !doc.isObject() || user != current_)
            return;
        for (const QJsonObject &m : messagesOf(doc))
            msgs_.insert(m.value("id").toString(), m); // (what is already held is not held twice: it has the same id)
        for (const QJsonObject &n : noticesOf(doc))
            notices_.insert(n.value("id").toString(), n);
        const QJsonValue next = doc.object().value("next");
        olderNext_ = next.isNull() ? -1 : qint64(next.toDouble());
        loadedOlder_ = true;
        renderMessages();
    });
}

// What the window shows: everything held, in the order it happened, with the notices between the messages.
void Chats::renderMessages()
{
    QList<QJsonObject> list = msgs_.values();
    std::sort(list.begin(), list.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const double ta = a.value("ts").toDouble(), tb = b.value("ts").toDouble();
        return ta != tb ? ta < tb : a.value("id").toString() < b.value("id").toString();
    });
    QList<QJsonObject> rows;
    QHash<QString, QJsonObject> byId = msgs_;
    const QVariantMap here = meIn(currentPlace()); // how we look in this conversation
    // what the messages are, and what happened to the group between them, in the order it happened
    QList<QJsonObject> items;
    for (QJsonObject m : list) {
        m.insert("kind", QStringLiteral("message"));
        items.append(m);
    }
    for (QJsonObject n : notices_.values()) {
        if (!notice(n))
            continue;
        items.append(n);
    }
    // a call older than every message held waits until the page it falls in is loaded, so that loading it adds one run above
    // what is there (the list keeps its place) rather than messages among notes already shown
    const double oldest = list.isEmpty() ? 0 : list.first().value("ts").toDouble();
    for (const QJsonObject &n : callNotes_)
        if (olderNext_ < 0 || n.value("ts").toDouble() >= oldest)
            items.append(n);
    std::stable_sort(items.begin(), items.end(), [](const QJsonObject &a, const QJsonObject &b) { return a.value("ts").toDouble() < b.value("ts").toDouble(); });
    QJsonObject prev;
    for (QJsonObject m : items) {
        const qint64 ts = qint64(m.value("ts").toDouble());
        const bool first = prev.isEmpty();
        const bool newDay = first || QDateTime::fromMSecsSinceEpoch(ts).date() != QDateTime::fromMSecsSinceEpoch(qint64(prev.value("ts").toDouble())).date();
        m.insert("dayStart", newDay);
        if (m.value("kind").toString() == QLatin1String("notice")) {
            m.insert("grouped", false);
            prev = m;
            rows.append(m);
            continue;
        }
        const QString sender = m.value("sender").toString();
        const bool mine = m.value("outgoing").toBool();
        if (m.contains("delivery")) {
            const QJsonObject d = m.value("delivery").toObject();
            m.insert("deliveryState", d.value("state"));
            m.insert("deliveryReason", d.value("reason"));
            m.insert("deliveryNext", d.value("next_at"));
        }
        const QJsonValue author = m.value("author");
        m.insert("senderName", mine ? selfName(author) : !nameIn(author).isEmpty() ? nameIn(author) : nameOf(sender));
        m.insert("avatar", mine ? (!avatarIn(author).isEmpty() ? avatarIn(author) : here.value("avatar").toString())
                                : author.isObject() ? avatarIn(author) : avatars_.value(sender));
        m.insert("tint", mine ? here.value("colors").toList().value(0).toString() : colors_.value(sender));
        m.insert("grouped", !first && !newDay && prev.value("kind").toString() == QLatin1String("message") && prev.value("sender") == m.value("sender") &&
                                !m.contains("reply_to") && ts - qint64(prev.value("ts").toDouble()) < 7 * 60 * 1000);
        if (m.contains("reply_to")) {
            const QJsonObject r = byId.value(m.value("reply_to").toString());
            if (r.isEmpty()) {
                m.insert("replyMissing", true);
            } else {
                m.insert("replyName", r.value("outgoing").toBool() ? selfName(r.value("author")) : !nameIn(r.value("author")).isEmpty() ? nameIn(r.value("author")) : nameOf(r.value("sender").toString()));
                const QString said = r.value("body").toString().simplified().left(100);
                const QJsonArray rf = r.value("files").toArray();
                m.insert("replyText", r.value("deleted").toBool() ? QString() : !said.isEmpty() || rf.isEmpty() ? said : rf.at(0).toObject().value("name").toString());
            }
        }
        prev = m;
        rows.append(m);
    }
    const bool older = olderNext_ >= 0;
    if (older != hasOlder_) {
        hasOlder_ = older;
        emit hasOlderChanged();
    }
    messages_.setItems(rows);
}

void Chats::open(const QString &userId, const QString &kind)
{
    if (current_ == userId)
        return;
    current_ = userId;
    currentKind_ = userId.isEmpty() ? QStringLiteral("dm") : kind;
    msgs_.clear();
    notices_.clear();
    callNotes_.clear();
    olderNext_ = -1;
    loadedOlder_ = loadingOlder_ = bridging_ = false;
    messages_.clear();
    if (hasOlder_) {
        hasOlder_ = false;
        emit hasOlderChanged(); // (the window still thinks the chat before this one had more history)
    }
    updateCurrentInfo();
    rebuildMembers();
    emit currentChanged();
    if (userId.isEmpty())
        return;
    markRead();
    refreshMessages();
    refreshCallNotes();
}

// The calls with the friend whose chat is open, as notes among the messages: "You called Bob · 3:12", "Missed call from Bob";
// or the group's: "Alice started a call · 12:40".
void Chats::refreshCallNotes()
{
    if (current_.isEmpty()) {
        callNotes_.clear();
        return;
    }
    const QString user = current_;
    const bool group = currentIsGroup();
    api_.get(QStringLiteral("/api/v1/calls?limit=200&") + (group ? QStringLiteral("group=") : QStringLiteral("user=")) + user,
             [this, user, group](int status, const QJsonDocument &doc) {
        if (status != 200 || user != current_)
            return;
        QHash<QString, QJsonObject> notes;
        for (const QJsonValue &v : doc.object().value("items").toArray()) {
            const QJsonObject c = v.toObject();
            if (c.value("state").toString() != QLatin1String("ended"))
                continue;
            const qint64 answered = qint64(c.value("answered").toDouble()), ended = qint64(c.value("ended").toDouble());
            const QString id = QStringLiteral("call:") + c.value("id").toString();
            if (notes.contains(id))
                continue; /* a group call left and joined again: the newest says it best */
            QJsonObject n;
            n.insert("id", id);
            n.insert("kind", QStringLiteral("notice"));
            n.insert("call", true);
            n.insert("ts", c.value("created"));
            const bool incoming = c.value("direction").toString() == QLatin1String("incoming");
            const qint64 seconds = answered > 0 && ended > answered ? (ended - answered) / 1000 : 0;
            n.insert("noticeText", group ? calls_.describeGroup(c.value("reason").toString(), incoming, nameOf(c.value("user").toString()), seconds)
                                         : calls_.describe(c.value("reason").toString(), incoming, nameOf(user), seconds));
            notes.insert(id, n);
        }
        if (notes.keys() == callNotes_.keys())
            return;
        callNotes_ = notes;
        renderMessages();
    });
}

QVariantMap Chats::currentPondDm() const
{
    if (!currentIsPondDm() || !pondDms_.contains(current_))
        return {};
    const QJsonObject c = pondDms_.value(current_);
    const QJsonObject pond = c.value("pond").toObject(), who = c.value("recipient").toObject();
    return {
        { "pond", pond.value("id").toString() },
        { "pondName", pond.value("name").toString() },
        { "pondIcon", ponds_.iconOf(pond.value("id").toString()) },
        { "member", who.value("id").toString() },
        { "memberName", who.value("display_name").toString() },
        { "closed", c.value("closed").toString() },
        { "offer", c.value("friend_offer").toString() },
        { "offered", c.value("offered").toBool() },
        { "blocked", c.value("blocked").toBool() },
        { "friend", c.value("friend").toBool() },
    };
}

void Chats::updateCurrentInfo()
{
    QString name, avatar, color, state;
    if (currentIsPondDm()) {
        const QJsonObject c = pondDms_.value(current_);
        name = c.value("name").toString();
        avatar = c.value("avatar").toString();
        color = c.value("color").toString();
        state = c.isEmpty() ? QString() : c.value("closed").isNull() ? QStringLiteral("active") : QStringLiteral("closed");
        curName_ = name; curAvatar_ = avatar; curColor_ = color; curState_ = state;
        emit currentInfoChanged(); // the banner's state changes without the name changing
        return;
    }
    if (currentIsGroup() && groups_.contains(current_)) {
        const QJsonObject g = groups_.value(current_);
        name = g.value("name").toString();
        state = g.value("my").toString() == QLatin1String("in") ? QStringLiteral("active") : g.value("my").toString(); // "active" is "can write here"
        if (name == curName_ && state == curState_ && curAvatar_.isEmpty() && curColor_.isEmpty())
            return;
        curName_ = name; curAvatar_.clear(); curColor_.clear(); curState_ = state;
        emit currentInfoChanged();
        return;
    }
    for (const QJsonObject &p : people_.items())
        if (p.value("id").toString() == current_) {
            name = p.value("name").toString();
            avatar = p.value("avatar").toString();
            color = p.value("color").toString();
            state = p.value("state").toString();
        }
    if (name.isEmpty() && !current_.isEmpty())
        name = shortId(current_);
    if (name == curName_ && avatar == curAvatar_ && color == curColor_ && state == curState_)
        return;
    curName_ = name; curAvatar_ = avatar; curColor_ = color; curState_ = state;
    emit currentInfoChanged();
}

void Chats::setAttentive(bool a)
{
    if (attentive_ == a)
        return;
    attentive_ = a;
    emit attentiveChanged();
    if (a)
        markRead(); // they are looking again: what arrived meanwhile is seen
}

void Chats::close()
{
    open(QString());
}

void Chats::post(const QString &path, const QJsonObject &body, const QString &failure, bool andRefresh)
{
    api_.post(path, body, [this, failure, andRefresh](int status, const QJsonDocument &) {
        if (status == 0 || status >= 400)
            emit failed(failure);
        if (andRefresh)
            refresh();
    });
}

void Chats::retry()
{
    if (current_.isEmpty())
        return;
    post(path("conversations", current_, "retry"), QJsonObject{}, tr("There was nothing to send again."));
}

void Chats::send(const QString &text, const QString &replyTo)
{
    const QString body = text.trimmed();
    if (current_.isEmpty() || body.isEmpty())
        return;
    QJsonObject o{ { "body", body } };
    if (!replyTo.isEmpty())
        o.insert("reply_to", replyTo);
    post(path("conversations", current_, "messages/send"), o, tr("The message could not be sent."));
}

// ---- sending files ----

struct Chats::FileSend {
    QString conversation, text, replyTo;
    QString path;              // where the message goes, if not to `conversation` (a Pond channel)
    QVariantList files;        // as sendFiles was given them
    QStringList ids;           // each one's id, once uploaded
};

static constexpr qint64 kChunk = 1 << 20; // the Puddle's chunk size (storage/files.h)

void Chats::sendFiles(const QString &text, const QString &replyTo, const QVariantList &files)
{
    if (files.isEmpty())
        return send(text, replyTo);
    if (current_.isEmpty())
        return;
    auto job = std::make_shared<FileSend>();
    job->conversation = current_; // the chat it was written in, even if the owner opens another meanwhile
    job->text = text.trimmed();
    job->replyTo = replyTo;
    job->files = files;
    uploadFile(job, 0);
}

void Chats::sendFilesTo(const QString &path, const QString &text, const QVariantList &files)
{
    auto job = std::make_shared<FileSend>();
    job->path = path;
    job->text = text.trimmed();
    job->files = files;
    uploadFile(job, 0);
}

void Chats::uploadFile(std::shared_ptr<FileSend> job, int index)
{
    if (index == job->files.size())
        return sendWithFiles(job);
    const QVariantMap f = job->files.at(index).toMap();
    if (takeFile_ && takePool_)
        return takeFile(job, index);
    const qint64 size = QFileInfo(f.value("path").toString()).size();
    if (size <= 0) {
        emit filesFailed(tr("%1 is empty or cannot be read.").arg(f.value("name").toString()));
        return;
    }
    api_.post(QStringLiteral("/api/v1/files/uploads/create"), QJsonObject{ { "size", size } }, [this, job, index, f, size](int status, const QJsonDocument &doc) {
        if (status != 201) {
            const QString code = doc.object().value("error").toString();
            emit filesFailed(code == QLatin1String("too_large") ? tr("%1 is larger than your Puddle takes.").arg(f.value("name").toString())
                                                               : tr("%1 could not be sent.").arg(f.value("name").toString()));
            return;
        }
        uploadChunk(job, index, doc.object().value("upload").toString(), 0, int((size + kChunk - 1) / kChunk));
    });
}

void Chats::takeFile(std::shared_ptr<FileSend> job, int index)
{
    const QVariantMap f = job->files.at(index).toMap();
    const QString path = f.value("path").toString();
    TakeFile fn = takeFile_;
    // the pool is waited for before this object goes (Backend's destructor), so `this` is still here when the answer comes
    takePool_->start([this, job, index, f, path, fn] {
        char id[65] = { 0 };
        int r = -1;
        QFile file(path);
        if (file.open(QIODevice::ReadOnly))
            r = fn(file.handle(), id);
        const QString sid = QString::fromLatin1(id);
        QMetaObject::invokeMethod(this, [this, job, index, f, r, sid] {
            const QString name = f.value("name").toString();
            if (r != 0) {
                emit filesFailed(r == -2 ? tr("%1 is empty, or larger than your Puddle takes.").arg(name) : tr("%1 could not be read.").arg(name));
                return;
            }
            emit uploadProgress(f.value("key").toString(), 1.0);
            job->ids << sid;
            uploadFile(job, index + 1);
        }, Qt::QueuedConnection);
    });
}

void Chats::uploadChunk(std::shared_ptr<FileSend> job, int index, const QString &token, int n, int count)
{
    const QVariantMap f = job->files.at(index).toMap();
    const QString key = f.value("key").toString();
    QFile file(f.value("path").toString());
    QByteArray bytes;
    if (file.open(QIODevice::ReadOnly) && file.seek(n * kChunk))
        bytes = file.read(kChunk);
    if (bytes.isEmpty()) {
        emit filesFailed(tr("%1 could not be read.").arg(f.value("name").toString()));
        return;
    }
    const QString upload = QStringLiteral("/api/v1/files/uploads/") + token;
    api_.postBytes(upload + QStringLiteral("/chunks/%1").arg(n), bytes, [this, job, index, token, n, count, key, upload, f](int status, const QJsonDocument &) {
        if (status != 200) {
            emit filesFailed(tr("%1 could not be sent.").arg(f.value("name").toString()));
            return;
        }
        emit uploadProgress(key, double(n + 1) / count);
        if (n + 1 < count)
            return uploadChunk(job, index, token, n + 1, count);
        api_.post(upload + QStringLiteral("/finish"), QJsonObject{}, [this, job, index, f](int status, const QJsonDocument &doc) {
            if (status != 201) {
                emit filesFailed(tr("%1 could not be sent.").arg(f.value("name").toString()));
                return;
            }
            job->ids << doc.object().value("id").toString();
            uploadFile(job, index + 1);
        });
    });
}

void Chats::sendWithFiles(std::shared_ptr<FileSend> job)
{
    QJsonArray files;
    for (int i = 0; i < job->files.size(); i++) {
        const QVariantMap f = job->files.at(i).toMap();
        QJsonObject o{ { "id", job->ids.at(i) }, { "name", f.value("name").toString() }, { "mime", f.value("mime").toString() } };
        if (f.value("w").toInt() > 0 && f.value("h").toInt() > 0) {
            o.insert("w", f.value("w").toInt());
            o.insert("h", f.value("h").toInt());
        }
        if (f.value("duration_ms").toLongLong() > 0)
            o.insert("duration_ms", f.value("duration_ms").toLongLong());
        if (!f.value("preview").toString().isEmpty())
            o.insert("preview", f.value("preview").toString());
        files.append(o);
    }
    QJsonObject o{ { "files", files } };
    if (!job->text.isEmpty())
        o.insert("body", job->text);
    if (!job->replyTo.isEmpty())
        o.insert("reply_to", job->replyTo);
    const QString to = job->path.isEmpty() ? path("conversations", job->conversation, "messages/send") : job->path;
    api_.post(to, o, [this, job](int status, const QJsonDocument &doc) {
        if (status != 201 && status != 200) {
            const QString code = doc.object().value("error").toString();
            emit filesFailed(code == QLatin1String("no_stream")   ? tr("You are not connected to them right now.")
                             : code == QLatin1String("too_large") ? tr("The Pond does not take files that large.")
                             : code == QLatin1String("files_off") ? tr("This Pond does not take files.")
                                                                  : tr("The message could not be sent."));
            return;
        }
        emit filesSent();
        if (job->path.isEmpty())
            refresh();
    });
}

void Chats::edit(const QString &messageId, const QString &text)
{
    post(path("conversations", current_, QStringLiteral("messages/%1/edit").arg(messageId)), QJsonObject{ { "body", text.trimmed() } },
         tr("The message could not be changed."));
}

void Chats::remove(const QString &messageId)
{
    post(path("conversations", current_, QStringLiteral("messages/%1/delete").arg(messageId)), QJsonObject{}, tr("The message could not be deleted."));
}

// who they will see us as, if the owner chose: nothing goes without that choice
static QJsonObject withProfile(QJsonObject o, const QString &profile)
{
    if (!profile.isEmpty())
        o.insert("profile", profile);
    return o;
}

void Chats::accept(const QString &u, const QString &profile) { post(path("streams", u, "accept"), withProfile({}, profile), tr("Could not accept.")); }
void Chats::decline(const QString &u) { post(path("streams", u, "decline"), QJsonObject{}, tr("Could not decline.")); }
void Chats::cancel(const QString &u) { post(path("streams", u, "delete"), QJsonObject{}, tr("Could not remove.")); }
void Chats::openPondDm(const QString &pond, const QString &member)
{
    api_.post(QStringLiteral("/api/v1/ponds/%1/dms/%2/open").arg(pond, member), QJsonObject{}, [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            const QString code = doc.object().value("error").toString();
            emit failed(code == QLatin1String("not_accepted") ? tr("They don't take direct messages in this Pond.")
                        : code == QLatin1String("dms_off") ? tr("This Pond doesn't pass on direct messages.")
                                                           : tr("The conversation could not be opened."));
            return;
        }
        QJsonObject c = doc.object();
        const QString id = c.value("id").toString();
        const QJsonObject who = c.value("recipient").toObject();
        c.insert("name", who.value("display_name").toString() + QStringLiteral(" @ ") + c.value("pond").toObject().value("name").toString());
        c.insert("avatar", who.value("avatar").toObject().value("hash"));
        c.insert("user", who.value("id"));
        pondDms_.insert(id, c);
        names_.insert(id, c.value("name").toString());
        open(id, QStringLiteral("pond_dm"));
        emit pondDmOpened(id);
        refreshConversations();
    });
}

void Chats::befriend()
{
    if (currentIsPondDm())
        post(path("conversations", current_, "befriend"), QJsonObject{}, tr("The offer could not be sent."));
}

void Chats::acceptFriendOffer()
{
    const QString link = currentPondDm().value("offer").toString();
    if (!link.isEmpty())
        request(link, QString());
}

void Chats::dmBlock(const QString &u) { post(path("users", u, "dm-block"), QJsonObject{}, tr("Could not block their messages.")); }
void Chats::dmUnblock(const QString &u) { post(path("users", u, "dm-unblock"), QJsonObject{}, tr("Could not unblock their messages.")); }

bool Chats::isBlocked(const QString &userId) const
{
    for (const QJsonObject &b : blocked_.items())
        if (b.value("user").toString() == userId)
            return true;
    return false;
}

void Chats::block(const QString &u) { post(path("users", u, "block"), QJsonObject{}, tr("Could not block.")); }
void Chats::unblock(const QString &u) { post(path("users", u, "unblock"), QJsonObject{}, tr("Could not unblock.")); }

void Chats::request(const QString &link, const QString &note, const QString &profile)
{
    QJsonObject o{ { "target", link } };
    if (!note.isEmpty())
        o.insert("message", note);
    post(QStringLiteral("/api/v1/streams/request"), withProfile(o, profile), tr("The request could not be sent. Check the link."));
}

// ---- groups ----

// the line a chat shows between the messages for something that happened to the group; false for a kind we do not show
bool Chats::notice(QJsonObject &n) const
{
    const QString type = n.value("type").toString(), who = n.value("sender").toString();
    const QString by = who == self_ ? tr("You") : nameOf(who);
    QString text;
    if (type == QLatin1String("create"))
        text = tr("%1 made the group “%2”").arg(by, n.value("name").toString());
    else if (type == QLatin1String("rename"))
        text = tr("%1 named the group “%2”").arg(by, n.value("name").toString());
    else if (type == QLatin1String("invite")) {
        const QString m = n.value("member").toString();
        text = tr("%1 invited %2").arg(by, m == self_ ? tr("you") : nameOf(m));
    } else if (type == QLatin1String("join"))
        text = tr("%1 joined").arg(by);
    else if (type == QLatin1String("leave"))
        text = tr("%1 left").arg(by);
    else
        return false;
    n.insert("kind", QStringLiteral("notice"));
    n.insert("noticeText", text);
    return true;
}

// The window's one list. First what is waiting for an answer (invitations to groups, friend requests), then every friend and every group,
// whether or not anything has been said, newest first (a friend or group nobody has written in yet counts from when it began), and last
// the requests we sent that nobody has answered. Blocked people and people we only know through a group are not in it.
void Chats::rebuildSidebar()
{
    struct Row {
        int rank;
        qint64 ts;
        QString name;
        QJsonObject o;
    };
    QList<Row> rows;
    auto add = [&rows](int rank, qint64 ts, const QJsonObject &o) { rows.append({ rank, ts, o.value("name").toString().toLower(), o }); };

    for (const QJsonObject &i : invites_.items()) {
        QJsonObject o;
        o.insert("key", QStringLiteral("invite:") + i.value("id").toString());
        o.insert("kind", QStringLiteral("invite"));
        o.insert("id", i.value("id"));
        o.insert("name", i.value("name"));
        o.insert("invitedBy", i.value("invitedByName"));
        o.insert("preview", tr("%1 invited you").arg(i.value("invitedByName").toString()));
        o.insert("section", QStringLiteral("pending"));
        add(0, 0, o);
    }
    for (const QJsonObject &p : people_.items()) {
        if (p.value("state").toString() != QLatin1String("pending_in"))
            continue;
        const QString id = p.value("id").toString(), note = p.value("note").toString();
        QJsonObject o;
        o.insert("key", QStringLiteral("request:") + id);
        o.insert("kind", QStringLiteral("request"));
        o.insert("id", id);
        o.insert("name", p.value("name"));
        o.insert("avatar", p.value("avatar"));
        o.insert("color", p.value("color"));
        o.insert("note", note);
        o.insert("preview", note.isEmpty() ? tr("Wants to be your friend") : QStringLiteral("“") + note + QStringLiteral("”"));
        o.insert("section", QStringLiteral("pending"));
        add(0, 0, o);
    }

    QSet<QString> haveChat;
    for (const QJsonObject &c : conversations_.items()) {
        const bool isGroup = c.value("kind").toString() == QLatin1String("group");
        const bool isPondDm = c.value("kind").toString() == QLatin1String("pond_dm");
        const QString id = c.value("id").toString();
        haveChat.insert(id);
        const QJsonObject last = c.value("last").toObject();
        QJsonObject o;
        o.insert("key", (isGroup ? QStringLiteral("group:") : isPondDm ? QStringLiteral("pdm:") : QStringLiteral("dm:")) + id);
        o.insert("kind", isGroup ? QStringLiteral("group") : isPondDm ? QStringLiteral("pond_dm") : QStringLiteral("dm"));
        if (isPondDm) {
            const QString pond = c.value("pond").toObject().value("id").toString();
            o.insert("pond", pond);
            o.insert("pondName", c.value("pond").toObject().value("name"));
            o.insert("pondIcon", ponds_.iconOf(pond));
            o.insert("user", c.value("user"));
        }
        o.insert("id", id);
        o.insert("name", c.value("name"));
        o.insert("avatar", c.value("avatar"));
        o.insert("color", c.value("color"));
        o.insert("unread", c.value("unread"));
        o.insert("state", isPondDm ? QJsonValue(c.value("closed").isNull() ? QStringLiteral("active") : QStringLiteral("closed")) : c.value("stream"));
        o.insert("members", c.value("members"));
        o.insert("section", QStringLiteral("chats"));
        QString preview;
        qint64 ts = 0;
        if (!last.isEmpty()) {
            ts = qint64(last.value("ts").toDouble());
            preview = last.value("deleted").toBool() ? tr("Message deleted")
                      : (isGroup ? c.value("lastBy").toString() + QStringLiteral(": ")
                                 : last.value("outgoing").toBool() ? tr("You: ") : QString()) + last.value("preview").toString();
        } else if (isPondDm && !c.value("friend_offer").isNull()) {
            preview = tr("Offers to be friends");
        } else if (isGroup) {
            ts = qint64(groups_.value(id).value("updated_at").toDouble());
            const int n = c.value("members").toInt();
            preview = n == 1 ? tr("1 member") : tr("%1 members").arg(n);
        }
        o.insert("preview", preview);
        o.insert("lastTs", double(ts));
        add(1, ts, o);
    }
    for (const QJsonObject &p : people_.items()) {
        const QString state = p.value("state").toString(), id = p.value("id").toString();
        if (state == QLatin1String("active") && !haveChat.contains(id)) {
            QJsonObject o;
            o.insert("key", QStringLiteral("dm:") + id);
            o.insert("kind", QStringLiteral("dm"));
            o.insert("id", id);
            o.insert("name", p.value("name"));
            o.insert("avatar", p.value("avatar"));
            o.insert("color", p.value("color"));
            o.insert("state", state);
            o.insert("preview", QString());
            o.insert("section", QStringLiteral("chats"));
            add(1, qint64(p.value("created_at").toDouble()), o);
        } else if (state == QLatin1String("pending_out")) {
            QJsonObject o;
            o.insert("key", QStringLiteral("sent:") + id);
            o.insert("kind", QStringLiteral("sent"));
            o.insert("id", id);
            o.insert("name", p.value("name"));
            o.insert("avatar", p.value("avatar"));
            o.insert("color", p.value("color"));
            o.insert("state", state);
            o.insert("preview", tr("Request sent"));
            o.insert("section", QStringLiteral("chats"));
            add(3, 0, o);
        }
    }

    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.rank != b.rank)
            return a.rank < b.rank;
        if (a.ts != b.ts)
            return a.ts > b.ts;
        return a.name.localeAwareCompare(b.name) < 0;
    });
    QList<QJsonObject> out;
    out.reserve(rows.size());
    for (const Row &r : rows)
        out.append(r.o);
    sidebar_.setItems(out);
}

void Chats::rebuildMembers()
{
    QList<QJsonObject> rows;
    const QVariantMap here = meIn(currentPlace());
    if (currentIsGroup() && groups_.contains(current_)) {
        QSet<QString> friends;
        for (const QJsonObject &p : people_.items())
            if (p.value("state").toString() == QLatin1String("active"))
                friends.insert(p.value("id").toString());
        for (const QJsonValue &v : groups_.value(current_).value("members").toArray()) {
            QJsonObject m = v.toObject();
            const QString id = idOf(m.value("user"));
            QJsonObject r;
            r.insert("id", id);
            const bool me = !self_.isEmpty() && id == self_;
            r.insert("name", me ? selfName(m.value("user")) : !nameIn(m.value("user")).isEmpty() ? nameIn(m.value("user")) : nameOf(id));
            r.insert("avatar", me ? (!avatarIn(m.value("user")).isEmpty() ? avatarIn(m.value("user")) : here.value("avatar").toString()) : avatarIn(m.value("user")));
            r.insert("color", me ? here.value("colors").toList().value(0).toString() : colors_.value(id));
            r.insert("state", m.value("state"));
            r.insert("isMe", me);
            r.insert("friend", friends.contains(id));
            rows.append(r);
        }
    }
    members_.setItems(rows);
}

void Chats::refreshGroups()
{
    if (busy_[5])
        return;
    busy_[5] = true;
    getAll(QStringLiteral("/api/v1/groups?limit=%1").arg(qMin(pageSize_, 100)), QStringLiteral("groups"), [this](int status, const QJsonDocument &doc) {
        busy_[5] = false;
        if (status != 200 || !doc.isObject())
            return;
        QHash<QString, QJsonObject> groups;
        QList<QJsonObject> invites;
        QSet<QString> invitedTo;
        for (const QJsonValue &v : doc.object().value("groups").toArray()) {
            const QJsonObject g = v.toObject();
            const QString id = g.value("id").toString();
            groups.insert(id, g);
            names_.insert(id, g.value("name").toString());
            if (g.value("my").toString() == QLatin1String("invited")) {
                invitedTo.insert(id);
                const QString by = idOf(g.value("invited_by"));
                QJsonObject row;
                row.insert("id", id);
                row.insert("name", g.value("name"));
                row.insert("invitedBy", by);
                row.insert("invitedByName", nameOf(by));
                invites.append(row);
                if (groupsLoaded_ && !invitedTo_.contains(id))
                    emit newGroupInvite(id);
            }
        }
        groups_ = groups;
        invitedTo_ = invitedTo;
        groupsLoaded_ = true;
        invites_.setItems(invites);
        rebuildSidebar();
        if (invites.size() != groupInviteCount_) {
            groupInviteCount_ = int(invites.size());
            emit groupInvitesChanged();
        }
        updateCurrentInfo();
        rebuildMembers();
    });
}

void Chats::createGroup(const QString &name, const QStringList &friends, const QString &profile)
{
    QJsonArray members;
    for (const QString &f : friends)
        members.append(f);
    api_.post(QStringLiteral("/api/v1/groups/create"), withProfile(QJsonObject{ { "name", name.trimmed() }, { "members", members } }, profile), [this](int status, const QJsonDocument &doc) {
        if (status != 201 || !doc.isObject()) {
            emit failed(tr("The group could not be made."));
            return;
        }
        if (!doc.object().value("not_invited").toArray().isEmpty())
            emit failed(tr("Some people could not be invited: they must be your friends."));
        emit groupCreated(doc.object().value("group").toObject().value("id").toString());
        refresh();
    });
}

void Chats::inviteToGroup(const QString &userId)
{
    if (!currentIsGroup())
        return;
    post(path("groups", current_, "invite"), QJsonObject{ { "user", userId } }, tr("They could not be invited."));
}

void Chats::acceptGroup(const QString &groupId, const QString &profile)
{
    post(path("groups", groupId, "accept"), withProfile({}, profile), tr("The group could not be joined."));
}

void Chats::declineGroup(const QString &groupId)
{
    post(path("groups", groupId, "decline"), QJsonObject{}, tr("Could not decline."));
}

void Chats::leaveGroup()
{
    if (currentIsGroup())
        leaveGroup(current_);
}

void Chats::leaveGroup(const QString &id)
{
    api_.post(path("groups", id, "leave"), QJsonObject{}, [this, id](int status, const QJsonDocument &) {
        if (status == 0 || status >= 400) {
            emit failed(tr("Could not leave the group."));
        } else {
            if (current_ == id)
                close();
            emit groupLeft();
        }
        refresh();
    });
}

void Chats::renameGroup(const QString &name)
{
    if (currentIsGroup())
        renameGroup(current_, name);
}

void Chats::renameGroup(const QString &id, const QString &name)
{
    post(path("groups", id, "rename"), QJsonObject{ { "name", name.trimmed() } }, tr("The group could not be renamed."));
}

QString Chats::selfName(const QJsonValue &author) const
{
    if (const QString n = nameIn(author); !n.isEmpty())
        return n;
    if (const QString n = meIn(currentPlace()).value("name").toString(); !n.isEmpty())
        return n;
    return tr("You");
}

QString Chats::currentPlace() const
{
    if (current_.isEmpty())
        return QString();
    if (currentIsGroup())
        return QStringLiteral("group:") + current_;
    if (currentIsPondDm())
        return QStringLiteral("pond:") + pondDms_.value(current_).value("pond").toObject().value("id").toString();
    return QStringLiteral("dm:") + current_;
}

QVariantMap Chats::meIn(const QString &place) const
{
    const auto it = places_.constFind(place);
    const QString name = it != places_.cend() && !it->first.isEmpty() ? it->first : myName_;
    const QString avatar = it != places_.cend() ? it->second : myAvatar_;
    /* the rest of the profile shown there: its colours (the first is our name's), banner, pronouns, blurb */
    QVariantMap c = Ponds::cardOf(placeProfiles_.value(place, myProfile_));
    c.insert("name", name);
    c.insert("avatar", avatar);
    return c;
}

QString Chats::nameOf(const QString &userId) const
{
    return names_.value(userId, shortId(userId));
}

// ---- the owner's profiles ----

// What went wrong, in words, from an error answer
static QString profileProblem(int status, const QJsonDocument &doc)
{
    const QJsonObject o = doc.object();
    const QString code = o.value("error").toString(), detail = o.value("detail").toString().isEmpty() ? o.value("message").toString()
                                                                                                     : o.value("detail").toString();
    if (status == 0)
        return QObject::tr("Can't reach your Puddle.");
    if (code == QLatin1String("exists"))
        return QObject::tr("There is already a profile with that id.");
    if (code == QLatin1String("too_many"))
        return QObject::tr("That's as many profiles as there can be.");
    if (code == QLatin1String("too_large"))
        return QObject::tr("That picture is too large: the most is 25 MiB.");
    if (code == QLatin1String("bad_image"))
        return QObject::tr("That isn't a picture Puddle can use: png, jpg, gif or webp.");
    if (!detail.isEmpty())
        return detail;
    return code.isEmpty() ? QObject::tr("Your Puddle said no (%1).").arg(status) : QObject::tr("Your Puddle said no: %1.").arg(code);
}

QVariantMap Chats::profile(const QString &id) const
{
    for (const QJsonObject &p : profiles_.items())
        if (p.value("id").toString() == id)
            return p.toVariantMap();
    return {};
}

void Chats::saveProfile(const QString &id, const QVariantMap &fields, bool isNew)
{
    QJsonObject body = QJsonObject::fromVariantMap(fields);
    if (isNew)
        body.insert("id", id);
    const QString path = isNew ? QStringLiteral("/api/v1/profiles/create") : QStringLiteral("/api/v1/profiles/%1/edit").arg(id);
    api_.post(path, body, [this, id](int status, const QJsonDocument &doc) {
        const QString why = status / 100 == 2 ? QString() : profileProblem(status, doc);
        if (why.isEmpty())
            refreshProfiles();
        emit profileSaved(id, why);
    });
}

void Chats::deleteProfile(const QString &id)
{
    api_.post(QStringLiteral("/api/v1/profiles/%1/delete").arg(id), QJsonObject(), [this, id](int status, const QJsonDocument &doc) {
        const QString why = status / 100 == 2 ? QString() : profileProblem(status, doc);
        if (why.isEmpty())
            refreshProfiles();
        emit profileDeleted(id, why);
    });
}

/* A picture made to fit where it goes: a Pond keeps none over 1 MiB (its docs/design.md, "Pictures"), so one larger, or larger
 * on screen than it is ever shown, is scaled down and saved again (PNG when it has see-through parts and that fits, else
 * JPEG), smaller until it fits. One that already fits is sent as it is, an animated GIF too. */
static QByteArray fitPicture(const QByteArray &in, const QString &slot)
{
    constexpr qint64 most = 1 << 20;
    const int side = slot == QLatin1String("banner") ? 1920 : slot == QLatin1String("background") ? 2048 : 1024;
    QBuffer src;
    src.setData(in);
    QImageReader reader(&src);
    const QSize size = reader.size();
    if (in.size() <= most && (!size.isValid() || qMax(size.width(), size.height()) <= side))
        return in;
    QImage img = reader.read();
    if (img.isNull())
        return in; /* not one Qt reads: the Puddle says what it thinks of it */
    if (qMax(img.width(), img.height()) > side)
        img = img.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    for (int round = 0; round < 8; round++) {
        QByteArray out;
        QBuffer b(&out);
        b.open(QIODevice::WriteOnly);
        if (img.hasAlphaChannel() && img.save(&b, "PNG") && out.size() <= most)
            return out;
        out.clear();
        b.seek(0);
        QImage flat(img.size(), QImage::Format_RGB32);
        flat.fill(Qt::white);
        { QPainter p(&flat); p.drawImage(0, 0, img); }
        if (flat.save(&b, "JPEG", 88 - round * 6) && out.size() <= most)
            return out;
        img = img.scaled(img.width() * 4 / 5, img.height() * 4 / 5, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return in;
}

void Chats::uploadPicture(const QUrl &file, const QString &slot)
{
    QFile f(file.isLocalFile() ? file.toLocalFile() : file.toString());
    if (!f.open(QIODevice::ReadOnly))
        return emit pictureUploaded(slot, QString(), tr("Couldn't read that file."));
    if (f.size() > 25LL * 1024 * 1024)
        return emit pictureUploaded(slot, QString(), tr("That picture is too large: the most is 25 MiB."));
    const QByteArray bytes = fitPicture(f.readAll(), slot);
    api_.postBytes(QStringLiteral("/api/v1/media/upload"), bytes, [this, slot, bytes](int status, const QJsonDocument &doc) {
        const QString hash = doc.object().value("hash").toString();
        if (status / 100 != 2 || hash.isEmpty())
            return emit pictureUploaded(slot, QString(), profileProblem(status, doc));
        // kept here too, so it shows at once without fetching it back
        QDir().mkpath(cacheDir_);
        QFile c(cacheDir_ + "/" + hash);
        if (!c.exists() && c.open(QIODevice::WriteOnly))
            c.write(bytes);
        emit pictureUploaded(slot, hash, QString());
    });
}

QString Chats::profileIdFor(const QString &name) const
{
    QString base;
    for (const QChar ch : name.toLower()) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')
            base += ch;
        else if (!base.isEmpty() && !base.endsWith('-'))
            base += '-';
    }
    while (base.endsWith('-'))
        base.chop(1);
    base = base.left(28);
    if (base.isEmpty())
        base = QStringLiteral("profile");
    auto taken = [this](const QString &id) {
        for (const QJsonObject &p : profiles_.items())
            if (p.value("id").toString() == id)
                return true;
        return false;
    };
    QString id = base;
    for (int n = 2; taken(id); n++)
        id = base + "-" + QString::number(n);
    return id;
}
