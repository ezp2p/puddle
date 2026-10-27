#include "chats.h"

#include <QClipboard>
#include <QCryptographicHash>
#include <QDateTime>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
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
      people_({ "id", "name", "state", "note", "error", "certificate_changed", "avatar", "color", "updated_at", "address", "provisional" }),
      conversations_({ "id", "kind", "user", "group", "name", "avatar", "color", "stream", "last", "lastBy", "unread", "members" }, nullptr, "id"),
      messages_({ "id", "sender", "senderName", "ts", "body", "outgoing", "edited", "deleted", "reply_to", "avatar", "tint", "grouped",
                  "dayStart", "replyName", "replyText", "replyMissing", "deliveryState", "deliveryReason", "deliveryNext", "kind", "noticeText" }),
      blocked_({ "id", "user", "name", "created_at" }, nullptr, "user"),
      profiles_({ "id", "display_name", "avatar", "default" }),
      invites_({ "id", "name", "invitedBy", "invitedByName" }),
      members_({ "id", "name", "avatar", "color", "state", "isMe", "friend" }),
      sidebar_({ "key", "kind", "id", "name", "avatar", "color", "preview", "lastTs", "unread", "state", "members", "note", "invitedBy", "section" }, nullptr, "key")
{
    cacheDir_ = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/avatars";
    timer_.setInterval(2000);
    connect(&timer_, &QTimer::timeout, this, &Chats::refresh);
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
    if (!profilesLoaded_)
        refreshProfiles();
}

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
            rows.append(b);
        }
        blocked_.setItems(rows);
    });
}

void Chats::refreshProfiles()
{
    if (busy_[4])
        return;
    busy_[4] = true;
    api_.get(QStringLiteral("/api/v1/profiles"), [this](int status, const QJsonDocument &doc) {
        busy_[4] = false;
        if (status != 200 || !doc.isObject())
            return;
        QList<QJsonObject> rows;
        const QJsonArray items = doc.object().value("items").toArray();
        for (const QJsonValue &v : items) {
            QJsonObject p = v.toObject();
            p.insert("avatar", p.value("avatar").toObject().value("hash").toString());
            p.insert("default", rows.isEmpty()); // the first is how the owner sees themselves
            if (rows.isEmpty()) {
                myName_ = p.value("display_name").toString();
                myAvatar_ = p.value("avatar").toString();
            }
            rows.append(p);
        }
        profilesLoaded_ = true;
        profiles_.setItems(rows);
        emit profilesChanged();
    });
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
                            { "address", p.value("address").toString() },
                            { "certChanged", p.value("certificate_changed").toBool() } };
    }
    return {};
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
            colors[id] = profile.value("color").toString();
            s.insert("name", name);
            s.insert("avatar", profile.value("avatar").toObject().value("hash").toString());
            s.insert("color", profile.value("color").toString());
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
            const QString user = c.value("id").toString(); // the id of the chat: who, or which group
            c.insert("last", c.value("last_message"));
            if (isGroup) {
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
        m.insert("senderName", mine ? tr("You") : !nameIn(author).isEmpty() ? nameIn(author) : nameOf(sender));
        m.insert("avatar", mine ? myAvatar_ : author.isObject() ? avatarIn(author) : avatars_.value(sender));
        m.insert("tint", mine ? QString() : colors_.value(sender));
        m.insert("grouped", !first && !newDay && prev.value("kind").toString() == QLatin1String("message") && prev.value("sender") == m.value("sender") &&
                                !m.contains("reply_to") && ts - qint64(prev.value("ts").toDouble()) < 7 * 60 * 1000);
        if (m.contains("reply_to")) {
            const QJsonObject r = byId.value(m.value("reply_to").toString());
            if (r.isEmpty()) {
                m.insert("replyMissing", true);
            } else {
                m.insert("replyName", r.value("outgoing").toBool() ? tr("You") : !nameIn(r.value("author")).isEmpty() ? nameIn(r.value("author")) : nameOf(r.value("sender").toString()));
                m.insert("replyText", r.value("deleted").toBool() ? QString() : r.value("body").toString().simplified().left(100));
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
}

void Chats::updateCurrentInfo()
{
    QString name, avatar, color, state;
    bool cert = false;
    if (currentIsGroup() && groups_.contains(current_)) {
        const QJsonObject g = groups_.value(current_);
        name = g.value("name").toString();
        state = g.value("my").toString() == QLatin1String("in") ? QStringLiteral("active") : g.value("my").toString(); // "active" is "can write here"
        if (name == curName_ && state == curState_ && curAvatar_.isEmpty() && curColor_.isEmpty() && !curCert_)
            return;
        curName_ = name; curAvatar_.clear(); curColor_.clear(); curState_ = state; curCert_ = false;
        emit currentInfoChanged();
        return;
    }
    for (const QJsonObject &p : people_.items())
        if (p.value("id").toString() == current_) {
            name = p.value("name").toString();
            avatar = p.value("avatar").toString();
            color = p.value("color").toString();
            state = p.value("state").toString();
            cert = p.value("certificate_changed").toBool();
        }
    if (name.isEmpty() && !current_.isEmpty())
        name = shortId(current_);
    if (name == curName_ && avatar == curAvatar_ && color == curColor_ && state == curState_ && cert == curCert_)
        return;
    curName_ = name; curAvatar_ = avatar; curColor_ = color; curState_ = state; curCert_ = cert;
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
void Chats::block(const QString &u) { post(path("users", u, "block"), QJsonObject{}, tr("Could not block.")); }
void Chats::unblock(const QString &u) { post(path("users", u, "unblock"), QJsonObject{}, tr("Could not unblock.")); }
void Chats::trust(const QString &u) { post(path("streams", u, "trust"), QJsonObject{}, tr("Could not trust the new certificate.")); }

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
        const QString id = c.value("id").toString();
        haveChat.insert(id);
        const QJsonObject last = c.value("last").toObject();
        QJsonObject o;
        o.insert("key", (isGroup ? QStringLiteral("group:") : QStringLiteral("dm:")) + id);
        o.insert("kind", isGroup ? QStringLiteral("group") : QStringLiteral("dm"));
        o.insert("id", id);
        o.insert("name", c.value("name"));
        o.insert("avatar", c.value("avatar"));
        o.insert("color", c.value("color"));
        o.insert("unread", c.value("unread"));
        o.insert("state", c.value("stream"));
        o.insert("members", c.value("members"));
        o.insert("section", QStringLiteral("chats"));
        QString preview;
        qint64 ts = 0;
        if (!last.isEmpty()) {
            ts = qint64(last.value("ts").toDouble());
            preview = last.value("deleted").toBool() ? tr("Message deleted")
                      : (isGroup ? c.value("lastBy").toString() + QStringLiteral(": ")
                                 : last.value("outgoing").toBool() ? tr("You: ") : QString()) + last.value("preview").toString();
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
            r.insert("name", me ? tr("You") : !nameIn(m.value("user")).isEmpty() ? nameIn(m.value("user")) : nameOf(id));
            r.insert("avatar", me ? myAvatar_ : avatarIn(m.value("user")));
            r.insert("color", me ? QString() : colors_.value(id));
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

QString Chats::nameOf(const QString &userId) const
{
    return names_.value(userId, shortId(userId));
}
