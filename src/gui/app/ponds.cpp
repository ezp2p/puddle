#include "ponds.h"

#include <QDateTime>
#include <QRegularExpression>

#include <climits>

#include <algorithm>
#include <QJsonArray>
#include <QUrl>

Ponds::Ponds(ApiClient *api, QObject *parent)
    : QObject(parent),
      api_(api),
      list_({ "id", "name", "status", "address", "icon" }),
      channels_({ "id", "name", "topic", "kind", "voice", "restricted", "roles", "overrides" }),
      members_({ "id", "name", "color", "nameColor", "avatar", "isMe", "role", "muted", "mutedUntil", "roles", "group" }),
      messages_({ "id", "author", "name", "tint", "nameColor", "avatar", "text", "files", "ts", "edited", "mine", "grouped", "dayStart" }),
      bans_({ "member", "name", "by", "byName", "reason", "ts" }, nullptr, QStringLiteral("member")),
      invites_({ "token", "link", "uses", "maxUses", "expires", "created" }, nullptr, QStringLiteral("token")),
      roles_({ "id", "name", "count", "color", "hoist", "permissions", "editable" }),
      pages_({ "id", "title", "kind", "updated", "restricted", "roles" }),
      orders_({ "id", "item", "title", "quantity", "amount", "currency", "status", "url", "live", "created", "paid", "member", "memberName" }),
      applications_({ "member", "name", "answers", "status", "reason", "created" }, nullptr, QStringLiteral("member")),
      log_({ "id", "author", "name", "text", "ts", "edited", "deleted", "deletedTs", "deletedBy", "deletedByName", "files", "removed", "versions" })
{
    timer_.setInterval(1000);
    connect(&timer_, &QTimer::timeout, this, &Ponds::refresh);
}

QString Ponds::channelName() const
{
    for (const QJsonObject &c : channels_.items())
        if (c.value("id").toString() == channel_)
            return c.value("name").toString();
    return QString();
}

QString Ponds::channelTopic() const
{
    for (const QJsonObject &c : channels_.items())
        if (c.value("id").toString() == channel_)
            return c.value("topic").toString();
    return QString();
}

QString Ponds::pathOfChannel() const
{
    return QStringLiteral("/api/v1/ponds/%1/channels/%2/messages").arg(current_, channel_);
}

// What to tell the owner when a request did not work.
QString Ponds::why(int status, const QJsonDocument &json, const QString &doing) const
{
    const QString code = json.object().value("error").toString();
    if (status == 0)
        return tr("Can't reach your Puddle.");
    if (code == QLatin1String("invite_refused"))
        return tr("That invite doesn't work any more. Ask for a new one.");
    if (code == QLatin1String("bad_link"))
        return tr("That isn't a Pond invite. It starts pond://");
    if (code == QLatin1String("unreachable"))
        return tr("The Pond can't be reached right now.");
    if (code == QLatin1String("not_a_pond"))
        return tr("Something answered at that address, but it isn't a Pond.");
    if (code == QLatin1String("removed"))
        return tr("You are no longer a member of this Pond.");
    if (code == QLatin1String("muted"))
        return tr("You're muted in this Pond, so you can't write here for now.");
    if (code == QLatin1String("slow_down"))
        return tr("You're sending messages too quickly. Wait a moment.");
    if (code == QLatin1String("not_proven"))
        return tr("Something answered for the Pond without proving it's the Pond, so nothing was sent.");
    if (code == QLatin1String("no_edits"))
        return tr("This Pond is too old to edit or delete messages.");
    if (code == QLatin1String("busy"))
        return tr("You're already in a call.");
    if (code == QLatin1String("full"))
        return tr("That voice channel is full.");
    if (code == QLatin1String("not_voice"))
        return tr("That isn't a voice channel.");
    if (code == QLatin1String("no_calls"))
        return tr("This Puddle was built without calls, so it can't join voice channels.");
    return tr("Couldn't %1.").arg(doing);
}

// A channel id comes as a number; the window keeps them as text, like every other id. A voice channel says who is in it, by
// name, as the members list knows them.
/* [1, 2] as ["1", "2"]: ids as QML compares them */
static QJsonArray idStrings(const QJsonValue &v)
{
    QJsonArray out;
    for (const QJsonValue &id : v.toArray())
        out.append(QString::number(qint64(id.toDouble())));
    return out;
}

QJsonObject Ponds::channelRow(const QJsonObject &c) const
{
    QJsonArray in;
    for (const QJsonValue &v : c.value("voice").toArray()) {
        const QJsonObject vo = v.toObject();
        const QString mid = vo.value("member").toString();
        const QJsonObject who = people_.value(mid);
        in.append(QJsonObject{ { "id", mid }, { "name", who.value("name").toString(mid.left(8)) }, { "color", who.value("color") },
                               { "avatar", who.value("avatar").toString() },
                               { "muted", vo.value("muted") }, { "deafened", vo.value("deafened") }, { "isMe", mid == me_ },
                               { "serverMuted", vo.value("server_muted").toBool() }, { "speak", vo.value("speak").toBool(true) } });
    }
    return { { "id", QString::number(qint64(c.value("id").toDouble())) }, { "name", c.value("name") }, { "topic", c.value("topic") },
             { "kind", c.value("kind").toString(QStringLiteral("text")) }, { "voice", in },
             { "restricted", c.value("roles").isArray() }, { "roles", idStrings(c.value("roles")) },
             { "overrides", c.value("overrides").toArray() } };
}

void Ponds::refresh()
{
    refreshVoice();
    // the open page has files on their way: look again every few seconds, until they are here
    if (!page_.isEmpty() && ++pageTicks_ % 3 == 0) {
        for (const QVariant &f : pageData_.value("files").toList()) {
            const QString st = f.toMap().value("state").toString();
            if (st == QLatin1String("downloading") || st == QLatin1String("waiting")) {
                loadPage();
                break;
            }
        }
    }
    if (busyList_ || !api_->ready())
        return;
    busyList_ = true;
    api_->get(QStringLiteral("/api/v1/ponds"), [this](int status, const QJsonDocument &json) {
        busyList_ = false;
        if (status != 200)
            return;
        const QJsonObject o = json.object();
        const qint64 v = qint64(o.value("version").toDouble());
        if (v == version_)
            return;
        version_ = v;
        QList<QJsonObject> rows;
        bool stillThere = false;
        for (const QJsonValue &p : o.value("items").toArray()) {
            const QJsonObject po = p.toObject();
            QHash<QString, QString> names;
            for (const QJsonValue &c : po.value("channels").toArray())
                names.insert(QString::number(qint64(c.toObject().value("id").toDouble())), c.toObject().value("name").toString());
            channelNames_.insert(po.value("id").toString(), names);
            QHash<QString, QStringList> in;
            for (const QJsonValue &c : po.value("channels").toArray()) {
                QList<QPair<int, QString>> bySlot;
                for (const QJsonValue &v : c.toObject().value("voice").toArray())
                    bySlot.append({ v.toObject().value("slot").toInt(), v.toObject().value("member").toString() });
                std::sort(bySlot.begin(), bySlot.end());
                QStringList ids;
                for (auto &[slot, id] : bySlot)
                    ids << id;
                in.insert(QString::number(qint64(c.toObject().value("id").toDouble())), ids);
            }
            voiceIn_.insert(po.value("id").toString(), in);
            pondNames_.insert(po.value("id").toString(), po.value("name").toString());
            rows.append({ { "id", po.value("id") }, { "name", po.value("name") }, { "status", po.value("status") }, { "address", po.value("address") },
                          { "icon", po.value("icon").toObject().value("hash").toString() } });
            if (po.value("id").toString() == current_)
                stillThere = true;
        }
        list_.setItems(rows);
        emit listChanged();
        updateVoicePeople();
        if (!current_.isEmpty() && !stillThere) { // we left it (here or on another device)
            close();
            return;
        }
        if (!current_.isEmpty()) {
            refreshPond();
            refreshMessages();
        }
    });
}

QString Ponds::iconOf(const QString &pondId) const
{
    for (const QJsonObject &p : list_.items())
        if (p.value("id").toString() == pondId)
            return p.value("icon").toString();
    return QString();
}

void Ponds::refreshPond()
{
    const QString id = current_;
    api_->get(QStringLiteral("/api/v1/ponds/") + id, [this, id](int status, const QJsonDocument &json) {
        if (status != 200 || id != current_)
            return;
        const QJsonObject o = json.object();
        me_ = o.value("me").toString();
        people_.clear();
        QList<QJsonObject> members;
        for (const QJsonValue &m : o.value("members").toArray()) {
            const QJsonObject mo = m.toObject(), prof = mo.value("profile").toObject();
            const QString mid = mo.value("id").toString();
            QString name = prof.value("display_name").toString();
            if (name.isEmpty())
                name = mid.left(8);
            const QString color = cardOf(prof).value("colors").toList().value(0).toString(); /* one colour, or a gradient's first */
            const QString avatar = prof.value("avatar").toObject().value("hash").toString();
            // muted by an operator or administrator, and not run out: until when (0: until unmuted)
            const QJsonValue mu = mo.value("muted");
            const double until = mu.isObject() ? mu.toObject().value("until").toDouble(0) : -1;
            const bool muted = mu.isObject() && (until == 0 || until > double(QDateTime::currentMSecsSinceEpoch()));
            people_.insert(mid, { { "name", name }, { "color", color }, { "avatar", avatar }, { "profile", prof }, { "role", mo.value("role").toString(QStringLiteral("member")) },
                                  { "muted", muted }, { "mutedUntil", muted ? until : -1 }, { "dms", mo.value("dms").toBool(true) } });
            members.append({ { "id", mid }, { "name", name }, { "color", color }, { "avatar", avatar }, { "isMe", mid == me_ },
                             { "muted", muted }, { "mutedUntil", muted ? until : -1 }, { "useRoleColor", mo.value("role_color").toBool() },
                             { "role", mo.value("role").toString(QStringLiteral("member")) }, { "roles", idStrings(mo.value("roles")) } });
        }
        // the roles, highest first, as the Pond keeps them
        QList<QJsonObject> roles;
        QStringList order, hoisted;
        for (const QJsonValue &r : o.value("roles").toArray()) {
            const QJsonObject ro = r.toObject();
            const QString rid = QString::number(qint64(ro.value("id").toDouble()));
            int count = 0;
            for (const QJsonObject &m : members)
                count += m.value("roles").toArray().contains(rid) ? 1 : 0;
            QStringList perms;
            for (const QJsonValue &p : ro.value("permissions").toArray())
                perms << p.toString();
            roles.append({ { "id", rid }, { "name", ro.value("name") }, { "count", count }, { "color", ro.value("color").toString() },
                           { "hoist", ro.value("hoist").toBool() }, { "permissions", QJsonArray::fromStringList(perms) } });
            order << rid;
            if (ro.value("hoist").toBool())
                hoisted << rid;
        }
        // the colour of a name (the Pond's docs/design.md, "Role colours"): their highest coloured role's, if they chose it or
        // the Pond makes everyone's; otherwise the colour of their profile, as they chose
        const bool enforced = o.value("role_colors").toString() == QLatin1String("enforced");
        for (QJsonObject &m : members) {
            QString c = m.value("color").toString();
            if (enforced || m.value("useRoleColor").toBool())
                for (const QJsonObject &r : roles)
                    if (!r.value("color").toString().isEmpty() && m.value("roles").toArray().contains(r.value("id"))) {
                        c = r.value("color").toString();
                        break;
                    }
            m.insert("nameColor", c);
            const QString mid = m.value("id").toString();
            QJsonObject who = people_.value(mid);
            who.insert("nameColor", c);
            who.insert("useRoleColor", m.value("useRoleColor"));
            people_.insert(mid, who);
        }
        // shown apart (Discord's "display separately"): each member under their highest role that is, in the roles' order, then
        // everyone else; operators first, then administrators, then by name within each
        auto group = [&](const QJsonObject &m) {
            int best = INT_MAX;
            for (const QJsonValue &r : m.value("roles").toArray())
                if (hoisted.contains(r.toString()))
                    best = std::min(best, int(order.indexOf(r.toString())));
            return best;
        };
        auto rank = [](const QJsonObject &m) { const QString r = m.value("role").toString(); return r == "operator" ? 0 : r == "administrator" ? 1 : 2; };
        std::stable_sort(members.begin(), members.end(), [&](const QJsonObject &a, const QJsonObject &b) {
            if (group(a) != group(b))
                return group(a) < group(b);
            if (rank(a) != rank(b))
                return rank(a) < rank(b);
            return a.value("name").toString().compare(b.value("name").toString(), Qt::CaseInsensitive) < 0;
        });
        if (!hoisted.isEmpty()) {
            QHash<int, int> counts;
            for (const QJsonObject &m : members)
                counts[group(m)]++;
            for (QJsonObject &m : members) {
                const int g = group(m);
                const QString title = g == INT_MAX ? tr("Members") : roles.at(g).value("name").toString();
                m.insert("group", QStringLiteral("%1 — %2").arg(title).arg(counts.value(g)));
            }
        }
        members_.setItems(members);
        roles_.setItems(roles);
        QList<QJsonObject> channels;
        for (const QJsonValue &c : o.value("channels").toArray())
            channels.append(channelRow(c.toObject()));
        channels_.setItems(channels);
        // the pages, in the Pond's order; the open one again if it changed, or the first channel if it went
        QList<QJsonObject> pages;
        bool pageThere = false;
        for (const QJsonValue &pv : o.value("pages").toArray()) {
            const QJsonObject po = pv.toObject();
            const QString pid = QString::number(qint64(po.value("id").toDouble()));
            pages.append({ { "id", pid }, { "title", po.value("title") }, { "kind", po.value("kind") }, { "updated", po.value("updated") },
                           { "restricted", po.value("roles").isArray() }, { "roles", idStrings(po.value("roles")) } });
            if (pid == page_) {
                pageThere = true;
                if (!pageData_.isEmpty() && qint64(po.value("updated").toDouble()) != qint64(pageData_.value("updated").toDouble()))
                    loadPage();
            }
        }
        pages_.setItems(pages);
        if (!page_.isEmpty() && !pageThere)
            closePage();
        info_ = o;
        // which roles we may change, give and move: below our highest, unless we have a rank
        const int mine = topOf(me_);
        const bool all = ranked(), manage = canManageRoles();
        for (int i = 0; i < roles.size(); i++)
            roles[i].insert("editable", manage && (all || i > mine));
        roles_.setItems(roles);
        emit currentInfoChanged();
        if (canManageApplications()) /* how many wait: a page and Server Settings say so */
            loadApplications();
        // the open channel was deleted, or none is open yet: the first there is
        bool have = false;
        for (const QJsonObject &c : channels)
            have = have || c.value("id").toString() == channel_;
        if (!have)
            openChannel(lastChannel_.contains(id) && std::any_of(channels.begin(), channels.end(), [&](const QJsonObject &c) {
                            return c.value("id").toString() == lastChannel_.value(id);
                        }) ? lastChannel_.value(id) : channels.isEmpty() ? QString() : channels.first().value("id").toString());
        else
            emit channelChanged(); /* its name or topic may have changed */
        rebuildMessages(); /* names may have changed */
    });
}

void Ponds::refreshMessages()
{
    if (current_.isEmpty() || channel_.isEmpty() || busyMessages_)
        return;
    busyMessages_ = true;
    const int gen = generation_;
    api_->get(pathOfChannel(), [this, gen](int status, const QJsonDocument &json) {
        busyMessages_ = false;
        if (gen != generation_ || status != 200)
            return;
        const QJsonObject o = json.object();
        QList<QJsonObject> page;
        for (const QJsonValue &m : o.value("items").toArray())
            page.append(m.toObject());
        // the newest page: anything newer than what is loaded is added; if it does not reach back to what is loaded, it replaces it
        const double newest = loaded_.isEmpty() ? 0 : loaded_.last().value("id").toDouble();
        const double oldestOfPage = page.isEmpty() ? 0 : page.first().value("id").toDouble();
        if (page.isEmpty()) { /* nothing in the channel now (the last of it deleted) */
            loaded_.clear();
            if (hasOlder_) {
                hasOlder_ = false;
                emit hasOlderChanged();
            }
        } else if (loaded_.isEmpty() || oldestOfPage > newest) {
            loaded_ = page;
            const bool older = o.value("more").toBool();
            if (older != hasOlder_) {
                hasOlder_ = older;
                emit hasOlderChanged();
            }
        } else {
            // the page's messages replace the ones loaded (what is here of their files changes), and newer ones are added
            QHash<double, QJsonObject> byId;
            for (const QJsonObject &m : page)
                byId.insert(m.value("id").toDouble(), m);
            for (QJsonObject &m : loaded_) {
                auto it = byId.constFind(m.value("id").toDouble());
                if (it != byId.cend())
                    m = *it;
            }
            /* one the page should hold and doesn't was deleted (protocol 14) */
            loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(), [&](const QJsonObject &m) {
                              const double id = m.value("id").toDouble();
                              return id >= oldestOfPage && !byId.contains(id);
                          }),
                          loaded_.end());
            for (const QJsonObject &m : page)
                if (m.value("id").toDouble() > newest)
                    loaded_.append(m);
        }
        rebuildMessages();
    });
}

void Ponds::rebuildMessages()
{
    QList<QJsonObject> rows;
    QJsonObject prev;
    for (const QJsonObject &m : loaded_) {
        const qint64 ts = qint64(m.value("ts").toDouble());
        const QString author = m.value("author").toString();
        if (blocked_.contains(author))
            continue; /* blocked: not shown here either (docs/pond-dms.md, "Blocks") */
        const QJsonObject who = people_.value(author);
        const bool first = prev.isEmpty();
        const bool newDay = first || QDateTime::fromMSecsSinceEpoch(ts).date() != QDateTime::fromMSecsSinceEpoch(qint64(prev.value("ts").toDouble())).date();
        const bool mine = author == me_;
        QJsonObject r{ { "id", QString::number(qint64(m.value("id").toDouble())) },
                       { "author", author },
                       { "name", !who.value("name").toString().isEmpty() ? who.value("name").toString() : mine ? tr("You") : author.left(8) },
                       { "tint", who.value("color").toString() },
                       { "nameColor", who.value("nameColor").toString(who.value("color").toString()) },
                       { "avatar", who.value("avatar").toString() },
                       { "text", m.value("text") },
                       { "files", m.value("files") },
                       { "ts", double(ts) },
                       { "edited", m.value("edited").toDouble(0) },
                       { "mine", mine },
                       { "dayStart", newDay },
                       { "grouped", !first && !newDay && prev.value("author").toString() == author && ts - qint64(prev.value("ts").toDouble()) < 7 * 60 * 1000 } };
        rows.append(r);
        prev = m;
    }
    messages_.setItems(rows);
}

QString Ponds::sendPath() const
{
    return channel_.isEmpty() ? QString() : pathOfChannel() + QStringLiteral("/send");
}

void Ponds::fetchFile(const QString &messageId, const QString &fileId)
{
    if (channel_.isEmpty() || messageId.isEmpty() || fileId.size() != 64)
        return;
    api_->post(pathOfChannel() + QStringLiteral("/%1/files/%2/download").arg(messageId, fileId), QJsonObject{},
               [this](int status, const QJsonDocument &json) {
                   if (status != 204)
                       emit failed(why(status, json, tr("fetch the file")));
                   else
                       refreshMessages();
               });
}

// ---- editing and deleting messages, and the message log (protocol 14) ----

void Ponds::editMessage(const QString &messageId, const QString &text, const QStringList &keepFiles)
{
    if (current_.isEmpty() || channel_.isEmpty() || messageId.isEmpty())
        return;
    QJsonArray keep;
    for (const QString &f : keepFiles)
        keep.append(f);
    const int gen = generation_;
    api_->post(pathOfChannel() + QStringLiteral("/%1/edit").arg(messageId), QJsonObject{ { "body", text.trimmed() }, { "files", keep } },
               [this, gen, messageId](int status, const QJsonDocument &json) {
                   if (status != 200) {
                       const QString code = json.object().value("error").toString();
                       emit failed(code == QLatin1String("bad_text") ? tr("A message needs some words, or a file.")
                                   : code == QLatin1String("no_message") ? tr("That message isn't there any more.")
                                                                          : why(status, json, tr("edit that")));
                       return;
                   }
                   emit messageEdited(messageId);
                   if (gen != generation_)
                       return;
                   const QJsonObject now = json.object();
                   for (QJsonObject &m : loaded_)
                       if (m.value("id").toDouble() == now.value("id").toDouble())
                           m = now;
                   rebuildMessages();
               });
}

void Ponds::deleteMessage(const QString &messageId)
{
    if (current_.isEmpty() || channel_.isEmpty() || messageId.isEmpty())
        return;
    const int gen = generation_;
    api_->post(pathOfChannel() + QStringLiteral("/%1/delete").arg(messageId), QJsonObject{}, [this, gen, messageId](int status, const QJsonDocument &json) {
        if (status != 204) {
            const QString code = json.object().value("error").toString();
            emit failed(code == QLatin1String("no_permission") ? tr("You may not delete that message.") : why(status, json, tr("delete that")));
            return;
        }
        if (gen != generation_)
            return;
        loaded_.erase(std::remove_if(loaded_.begin(), loaded_.end(),
                                     [&](const QJsonObject &m) { return QString::number(qint64(m.value("id").toDouble())) == messageId; }),
                      loaded_.end());
        rebuildMessages();
        if (!logLoaded_.isEmpty())
            loadLog();
    });
}

void Ponds::loadLog(bool older)
{
    if (current_.isEmpty() || channel_.isEmpty() || busyLog_ || (older && (!logHasOlder_ || logLoaded_.isEmpty())))
        return;
    busyLog_ = true;
    const int gen = generation_;
    QString path = QStringLiteral("/api/v1/ponds/%1/channels/%2/log").arg(current_, channel_);
    if (older)
        path += QStringLiteral("?before=") + QString::number(qint64(logLoaded_.last().value("id").toDouble()));
    api_->get(path, [this, gen, older](int status, const QJsonDocument &json) {
        busyLog_ = false;
        if (gen != generation_)
            return;
        if (status != 200) {
            emit failed(why(status, json, tr("read the message log")));
            return;
        }
        QList<QJsonObject> page;
        for (const QJsonValue &m : json.object().value("items").toArray())
            page.append(m.toObject());
        logLoaded_ = older ? logLoaded_ + page : page;
        logHasOlder_ = page.size() >= 50;
        rebuildLog();
        emit logChanged();
    });
}

void Ponds::loadVersions(const QString &messageId)
{
    if (current_.isEmpty() || channel_.isEmpty() || messageId.isEmpty())
        return;
    const int gen = generation_;
    api_->get(pathOfChannel() + QStringLiteral("/%1/versions").arg(messageId), [this, gen](int status, const QJsonDocument &json) {
        if (gen != generation_)
            return;
        if (status != 200) {
            emit failed(why(status, json, tr("read the message log")));
            return;
        }
        logLoaded_ = { json.object() };
        logHasOlder_ = false;
        rebuildLog();
        emit logChanged();
    });
}

void Ponds::rebuildLog()
{
    QList<QJsonObject> rows;
    for (const QJsonObject &m : logLoaded_) {
        const QString author = m.value("author").toString(), by = m.value("deleted").toObject().value("by").toString();
        auto name = [this](const QString &id) {
            return id == QLatin1String("pondctl") ? tr("The server's admin") : people_.value(id).value("name").toString(id.left(8));
        };
        rows.append({ { "id", QString::number(qint64(m.value("id").toDouble())) },
                      { "author", author },
                      { "name", name(author) },
                      { "text", m.value("text") },
                      { "ts", m.value("ts").toDouble() },
                      { "edited", m.value("edited").toDouble(0) },
                      { "deleted", m.contains("deleted") },
                      { "deletedTs", m.value("deleted").toObject().value("ts").toDouble(0) },
                      { "deletedBy", by },
                      { "deletedByName", by.isEmpty() ? QString() : name(by) },
                      { "files", m.value("files") },
                      { "removed", m.value("removed") },
                      { "versions", m.value("versions") } });
    }
    log_.setItems(rows);
}

void Ponds::purgeMessage(const QString &messageId)
{
    if (current_.isEmpty() || channel_.isEmpty() || messageId.isEmpty())
        return;
    api_->post(pathOfChannel() + QStringLiteral("/%1/purge").arg(messageId), QJsonObject{}, [this](int status, const QJsonDocument &json) {
        if (status != 204) {
            const QString code = json.object().value("error").toString();
            emit failed(code == QLatin1String("not_deleted") ? tr("Delete the message first.") : why(status, json, tr("erase that")));
            return;
        }
        emit adminDone(tr("Erased for good."));
        loadLog();
    });
}

void Ponds::fetchLogFile(const QString &messageId, const QString &fileId)
{
    if (channel_.isEmpty() || messageId.isEmpty() || fileId.size() != 64)
        return;
    api_->post(pathOfChannel() + QStringLiteral("/%1/files/%2/download").arg(messageId, fileId), QJsonObject{},
               [this](int status, const QJsonDocument &json) {
                   if (status != 204)
                       emit failed(why(status, json, tr("fetch the file")));
                   else
                       QTimer::singleShot(1500, this, [this] { loadLog(); }); /* what is here of it, a moment later */
               });
}

// ---- roles ----

void Ponds::answerOffer(bool accept)
{
    if (current_.isEmpty())
        return;
    const QString id = current_;
    api_->post(QStringLiteral("/api/v1/ponds/%1/offer/%2").arg(id, accept ? "accept" : "decline"), QJsonObject{},
               [this, accept](int status, const QJsonDocument &json) {
                   if (status != 200)
                       emit failed(json.object().value("error").toString() == QLatin1String("no_offer") ? tr("That offer has lapsed.")
                                                                                                    : why(status, json, tr("answer the offer")));
                   else if (accept)
                       emit adminDone(tr("You are now %1 here.").arg(json.object().value("role").toString() == QLatin1String("operator")
                                                                         ? tr("an operator") : tr("an administrator")));
                   refresh();
               });
}

void Ponds::admin(const QString &path, const QJsonObject &body, const QString &done, const QString &doing, bool reloadBans, bool reloadInvites)
{
    if (current_.isEmpty())
        return;
    api_->post(QStringLiteral("/api/v1/ponds/%1/admin/%2").arg(current_, path), body, [this, done, doing, reloadBans, reloadInvites](int status, const QJsonDocument &json) {
        if (status < 200 || status > 299) {
            const QJsonObject o = json.object();
            const QString code = o.value("error").toString();
            emit adminFailed(!o.value("message").toString().isEmpty() ? o.value("message").toString()
                             : code == QLatin1String("no_role")       ? tr("You may no longer do that in this Pond.")
                             : code == QLatin1String("unknown_role")  ? tr("That role was deleted meanwhile.")
                             : code == QLatin1String("channels_changed") ? tr("The channels changed meanwhile. Try again.")
                             : code == QLatin1String("outranked")     ? (ranked() ? tr("They are your rank or above.") : tr("That is above your highest role, or more than you may do."))
                             : code == QLatin1String("roles_changed") ? tr("The roles changed meanwhile. Try again.")
                             : code == QLatin1String("unknown_permission") ? tr("This Pond doesn't know that permission.")
                             : code == QLatin1String("not_yourself")  ? tr("You can't do that to yourself.")
                             : code == QLatin1String("too_large")     ? tr("That picture is too large for a Pond: the most is 1 MiB.")
                             : code == QLatin1String("no_pictures")   ? tr("This Pond is too old to have an icon.")
                                                                      : why(status, json, doing));
            return;
        }
        emit adminDone(done);
        if (reloadBans)
            loadBans();
        if (reloadInvites)
            loadInvites();
        refresh();
    });
}

void Ponds::saveOverview(const QString &name, const QString &description)
{
    admin(QStringLiteral("pond"), { { "name", name.trimmed() }, { "description", description.trimmed() } }, tr("Saved."), tr("save that"));
}

void Ponds::setIcon(const QString &hash)
{
    QJsonObject body;
    body.insert("hash", hash.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(hash));
    admin(QStringLiteral("icon"), body, hash.isEmpty() ? tr("Icon removed.") : tr("Icon changed."), tr("change the icon"));
}

void Ponds::createChannel(const QString &name, const QString &topic, const QString &kind)
{
    const bool voice = kind == QLatin1String("voice");
    admin(QStringLiteral("channels/create"),
          { { "name", name.trimmed() }, { "topic", topic.trimmed() }, { "kind", voice ? QStringLiteral("voice") : QStringLiteral("text") } },
          voice ? tr("Voice channel %1 made.").arg(name.trimmed()) : tr("#%1 made.").arg(name.trimmed()), tr("make that channel"));
}

void Ponds::editChannel(const QString &channelId, const QString &name, const QString &topic)
{
    admin(QStringLiteral("channels/%1/edit").arg(channelId), { { "name", name.trimmed() }, { "topic", topic.trimmed() } }, tr("Saved."),
          tr("change that channel"));
}

void Ponds::deleteChannel(const QString &channelId)
{
    admin(QStringLiteral("channels/%1/delete").arg(channelId), {}, tr("Channel deleted."), tr("delete that channel"));
}

void Ponds::createRole(const QString &name)
{
    admin(QStringLiteral("roles/create"), { { "name", name.trimmed() } }, tr("Role %1 made.").arg(name.trimmed()), tr("make that role"));
}

void Ponds::renameRole(const QString &roleId, const QString &name)
{
    admin(QStringLiteral("roles/%1/edit").arg(roleId), { { "name", name.trimmed() } }, tr("Saved."), tr("rename that role"));
}

void Ponds::editRole(const QString &roleId, const QVariantMap &changes)
{
    QJsonObject body;
    if (changes.contains("name"))
        body.insert("name", changes.value("name").toString().trimmed());
    if (changes.contains("color")) {
        const QString c = changes.value("color").toString().toLower();
        body.insert("color", c.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(c));
    }
    if (changes.contains("hoist"))
        body.insert("hoist", changes.value("hoist").toBool());
    if (changes.contains("permissions"))
        body.insert("permissions", QJsonArray::fromStringList(changes.value("permissions").toStringList()));
    if (body.isEmpty())
        return;
    admin(QStringLiteral("roles/%1/edit").arg(roleId), body, tr("Saved."), tr("change that role"));
}

void Ponds::moveRole(const QString &roleId, int by)
{
    QStringList order;
    for (const QJsonObject &r : roles_.items())
        order << r.value("id").toString();
    const int from = int(order.indexOf(roleId)), to = from + by;
    if (from < 0 || to < 0 || to >= order.size())
        return;
    order.move(from, to);
    QJsonArray ids;
    for (const QString &r : order)
        ids.append(r.toLongLong());
    admin(QStringLiteral("roles/order"), { { "roles", ids } }, tr("Moved."), tr("move that role"));
}

QVariantList Ponds::rolesOf(const QString &memberId) const
{
    QJsonArray held;
    for (const QJsonObject &m : members_.items())
        if (m.value("id").toString() == memberId)
            held = m.value("roles").toArray();
    QVariantList out;
    for (const QJsonObject &r : roles_.items())
        if (held.contains(r.value("id")))
            out << QVariantMap{ { "id", r.value("id").toString() }, { "name", r.value("name").toString() }, { "color", r.value("color").toString() } };
    return out;
}

QStringList Ponds::everyone() const
{
    const QJsonValue e = info_.value("everyone");
    if (!e.isObject()) /* a Pond before protocol 13: everyone could do what members always could */
        return { "view_channels", "send_messages", "attach_files", "connect", "speak", "video" };
    QStringList out;
    for (const QJsonValue &p : e.toObject().value("permissions").toArray())
        out << p.toString();
    return out;
}

bool Ponds::canAdmin() const
{
    if (ranked())
        return true;
    static const QSet<QString> admin{ "manage_pond", "manage_channels", "manage_roles", "manage_invites", "kick_members", "ban_members",
                                      "mute_members", "manage_pages", "manage_events", "manage_applications", "manage_shop" };
    return myPermissions().intersects(admin);
}

bool Ponds::mayIn(const QString &channelId, const QString &permission) const
{
    if (ranked())
        return true;
    QSet<QString> p = myPermissions();
    QJsonArray overrides;
    for (const QJsonObject &c : channels_.items())
        if (c.value("id").toString() == channelId)
            overrides = c.value("overrides").toArray();
    QJsonArray held;
    for (const QJsonObject &m : members_.items())
        if (m.value("id").toString() == me_)
            held = m.value("roles").toArray();
    // as the Pond works it out: everyone's override, then all of our roles' (an allow wins over a deny)
    QSet<QString> allow, deny;
    for (const QJsonValue &v : overrides) {
        const QJsonObject o = v.toObject();
        const QString role = QString::number(qint64(o.value("role").toDouble()));
        if (role == QLatin1String("0")) {
            for (const QJsonValue &d : o.value("deny").toArray())
                p.remove(d.toString());
            for (const QJsonValue &a : o.value("allow").toArray())
                p.insert(a.toString());
        } else if (held.contains(role)) {
            for (const QJsonValue &d : o.value("deny").toArray())
                deny.insert(d.toString());
            for (const QJsonValue &a : o.value("allow").toArray())
                allow.insert(a.toString());
        }
    }
    p.subtract(deny);
    p.unite(allow);
    return p.contains(permission);
}

QVariantList Ponds::channelOverrides(const QString &channelId) const
{
    for (const QJsonObject &c : channels_.items())
        if (c.value("id").toString() == channelId) {
            QVariantList out;
            for (const QJsonValue &v : c.value("overrides").toArray()) {
                QVariantMap o = v.toObject().toVariantMap();
                o.insert("role", QString::number(qint64(v.toObject().value("role").toDouble())));
                out << o;
            }
            return out;
        }
    return {};
}

QSet<QString> Ponds::myPermissions() const
{
    QJsonArray held;
    for (const QJsonObject &m : members_.items())
        if (m.value("id").toString() == me_)
            held = m.value("roles").toArray();
    QSet<QString> out;
    for (const QString &p : everyone())
        out.insert(p);
    for (const QJsonObject &r : roles_.items())
        if (held.contains(r.value("id")))
            for (const QJsonValue &p : r.value("permissions").toArray())
                out.insert(p.toString());
    return out;
}

int Ponds::topOf(const QString &memberId) const
{
    QJsonArray held;
    for (const QJsonObject &m : members_.items())
        if (m.value("id").toString() == memberId)
            held = m.value("roles").toArray();
    const QList<QJsonObject> roles = roles_.items();
    for (int i = 0; i < roles.size(); i++)
        if (held.contains(roles.at(i).value("id")))
            return i;
    return INT_MAX;
}

QString Ponds::memberRole(const QString &memberId) const
{
    for (const QJsonObject &m : members_.items())
        if (m.value("id").toString() == memberId)
            return m.value("role").toString();
    return QStringLiteral("member");
}

void Ponds::deleteRole(const QString &roleId)
{
    admin(QStringLiteral("roles/%1/delete").arg(roleId), {}, tr("Role deleted."), tr("delete that role"));
}

QString Ponds::roleName(const QString &roleId) const
{
    for (const QJsonObject &r : roles_.items())
        if (r.value("id").toString() == roleId)
            return r.value("name").toString();
    return QString();
}

void Ponds::setMemberRole(const QString &memberId, const QString &roleId, bool held)
{
    QJsonArray now;
    for (const QJsonObject &m : members_.items())
        if (m.value("id").toString() == memberId)
            now = m.value("roles").toArray();
    QJsonArray ids;
    for (const QJsonValue &r : now)
        if (r.toString() != roleId)
            ids.append(r.toString().toLongLong());
    if (held)
        ids.append(roleId.toLongLong());
    const QString who = nameOf(memberId), role = roleName(roleId);
    admin(QStringLiteral("members/%1/roles").arg(memberId), { { "roles", ids } },
          held ? tr("%1 has %2 now.").arg(who, role) : tr("%1 no longer has %2.").arg(who, role), tr("change their roles"));
}

void Ponds::setChannelRoles(const QString &channelId, bool everyone, const QStringList &roleIds)
{
    QJsonArray ids;
    for (const QString &r : roleIds)
        ids.append(r.toLongLong());
    QJsonObject body;
    body.insert("roles", everyone ? QJsonValue(QJsonValue::Null) : QJsonValue(ids));
    admin(QStringLiteral("channels/%1/roles").arg(channelId), body, tr("Saved."), tr("change who can see that channel"));
}

void Ponds::moveChannel(const QString &channelId, int by)
{
    // the order as we have it, with this one moved; the Pond takes it only if it has the same channels (else 409)
    QStringList order;
    for (const QJsonObject &c : channels_.items())
        order << c.value("id").toString();
    const int from = int(order.indexOf(channelId)), to = from + by;
    if (from < 0 || to < 0 || to >= order.size())
        return;
    order.move(from, to);
    QJsonArray ids;
    for (const QString &c : order)
        ids.append(c.toLongLong());
    admin(QStringLiteral("channels/order"), { { "channels", ids } }, tr("Moved."), tr("move that channel"));
}

QVariantMap Ponds::cardOf(const QJsonObject &profile)
{
    QVariantList colors; /* "color": one, as "#rrggbb", or {colors, angle} for a gradient */
    const QJsonValue color = profile.value("color");
    if (color.isString() && !color.toString().isEmpty())
        colors << color.toString();
    for (const QJsonValue &c : color.toObject().value("colors").toArray())
        colors << c.toString();
    return { { "name", profile.value("display_name").toString() }, { "avatar", profile.value("avatar").toObject().value("hash").toString() },
             { "banner", profile.value("banner").toObject().value("hash").toString() },
             { "blurb", profile.value("blurb").toString() }, { "pronouns", profile.value("pronouns").toString() }, { "colors", colors },
             { "angle", profile.value("color").toObject().value("angle").toInt(180) } };
}

QVariantMap Ponds::card(const QString &memberId) const
{
    const QJsonObject who = people_.value(memberId);
    QVariantMap c = cardOf(who.value("profile").toObject());
    c.insert("id", memberId);
    c.insert("name", nameOf(memberId));
    c.insert("role", who.value("role").toString(QStringLiteral("member")));
    c.insert("isMe", memberId == me_);
    c.insert("muted", who.value("muted").toBool());
    c.insert("mutedUntil", who.value("mutedUntil").toDouble(-1));
    c.insert("place", currentName());
    c.insert("pond", current_);
    c.insert("pondIcon", currentIcon());
    c.insert("dms", dmsOn() && who.value("dms").toBool(true));
    c.insert("roles", rolesOf(memberId)); /* the roles they hold there, highest first */ /* a direct message through this Pond can be written to them */
    return c;
}

void Ponds::setAcceptDms(bool accept)
{
    const QString id = current_;
    api_->post(QStringLiteral("/api/v1/ponds/%1/dms/settings").arg(id), QJsonObject{ { "accept", accept } }, [this, accept](int status, const QJsonDocument &) {
        if (status != 200)
            emit failed(tr("Could not change who may write to you here."));
        else
            emit adminDone(accept ? tr("Members can send you direct messages here.") : tr("Nobody can send you direct messages here."));
        refreshPond();
    });
}

void Ponds::mute(const QString &memberId, int seconds)
{
    QJsonObject body;
    if (seconds > 0)
        body.insert("until", double(QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000));
    admin(QStringLiteral("members/%1/mute").arg(memberId), body, tr("%1 is muted.").arg(nameOf(memberId)), tr("mute them"));
}

void Ponds::unmute(const QString &memberId)
{
    admin(QStringLiteral("members/%1/unmute").arg(memberId), {}, tr("%1 can talk again.").arg(nameOf(memberId)), tr("unmute them"));
}

void Ponds::kick(const QString &memberId)
{
    admin(QStringLiteral("members/%1/remove").arg(memberId), {}, tr("%1 was removed.").arg(people_.value(memberId).value("name").toString()),
          tr("remove them"));
}

void Ponds::ban(const QString &memberId, const QString &reason)
{
    admin(QStringLiteral("members/%1/ban").arg(memberId), { { "reason", reason.trimmed() } },
          tr("%1 was banned.").arg(people_.value(memberId).value("name").toString()), tr("ban them"), true);
}

void Ponds::unban(const QString &memberId)
{
    admin(QStringLiteral("bans/%1/unban").arg(memberId), {}, tr("Unbanned. They can join again with an invite."), tr("unban them"), true);
}

void Ponds::loadBans()
{
    if (current_.isEmpty())
        return;
    api_->get(QStringLiteral("/api/v1/ponds/%1/admin/bans").arg(current_), [this](int status, const QJsonDocument &json) {
        if (status != 200)
            return;
        QList<QJsonObject> rows;
        for (const QJsonValue &v : json.object().value("bans").toArray()) {
            const QJsonObject b = v.toObject();
            const QString id = b.value("member").toString(), by = b.value("by").toString();
            QString name = b.value("profile").toObject().value("display_name").toString();
            rows.append({ { "member", id }, { "name", name.isEmpty() ? id.left(8) : name }, { "by", by },
                          { "byName", by == QLatin1String("pondctl") ? tr("the server's admin") : people_.value(by).value("name").toString(by.left(8)) },
                          { "reason", b.value("reason").toString() }, { "ts", b.value("ts").toDouble() } });
        }
        bans_.setItems(rows);
    });
}

void Ponds::loadInvites()
{
    if (current_.isEmpty())
        return;
    const QString id = current_;
    api_->get(QStringLiteral("/api/v1/ponds/%1/admin/invites").arg(id), [this, id](int status, const QJsonDocument &json) {
        if (status != 200 || id != current_)
            return;
        QList<QJsonObject> rows;
        for (const QJsonValue &v : json.object().value("invites").toArray()) {
            const QJsonObject i = v.toObject();
            rows.append({ { "token", i.value("token") }, { "link", i.value("link") }, { "uses", i.value("uses").toInt() },
                          { "maxUses", i.value("max_uses").isNull() ? 0 : i.value("max_uses").toInt() },
                          { "expires", i.value("expires").isNull() ? 0.0 : i.value("expires").toDouble() }, { "created", i.value("created").toDouble() } });
        }
        invites_.setItems(rows);
    });
}

void Ponds::createInvite(int uses, int seconds)
{
    if (current_.isEmpty())
        return;
    QJsonObject body;
    if (uses > 0)
        body.insert("uses", uses);
    if (seconds > 0)
        body.insert("expires", double(QDateTime::currentMSecsSinceEpoch() + qint64(seconds) * 1000));
    api_->post(QStringLiteral("/api/v1/ponds/%1/admin/invites/create").arg(current_), body, [this](int status, const QJsonDocument &json) {
        if (status != 200) {
            const QString code = json.object().value("error").toString();
            emit adminFailed(code == QLatin1String("no_role") ? tr("You may no longer do that in this Pond.") : why(status, json, tr("make an invite")));
            return;
        }
        emit inviteMade(json.object().value("link").toString());
        loadInvites();
    });
}

void Ponds::revokeInvite(const QString &token)
{
    admin(QStringLiteral("invites/%1/revoke").arg(token), {}, tr("Invite revoked. It no longer works."), tr("revoke that invite"), false, true);
}

bool Ponds::canActOn(const QString &memberId) const
{
    auto rank = [](const QString &r) { return r == QLatin1String("operator") ? 2 : r == QLatin1String("administrator") ? 1 : 0; };
    const QString theirs = memberRole(memberId);
    if (memberId == me_ || theirs == QLatin1String("operator"))
        return false;
    if (ranked())
        return rank(currentRole()) > rank(theirs);
    // let by our roles: only on a member, whose highest role is below ours
    return (canKick() || canBan() || canMute()) && rank(theirs) == 0 && topOf(me_) < topOf(memberId);
}

void Ponds::join(const QString &link, const QString &profile, const QVariantMap &answers)
{
    if (joining_)
        return;
    joining_ = true;
    emit joiningChanged();
    QJsonObject body{ { "link", link.trimmed() } };
    body.insert("profile", profile.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(profile));
    if (!answers.isEmpty())
        body.insert("answers", QJsonObject::fromVariantMap(answers));
    api_->post(QStringLiteral("/api/v1/ponds/join"), body, [this, link, profile](int status, const QJsonDocument &json) {
        joining_ = false;
        emit joiningChanged();
        const QString code = json.object().value("error").toString();
        if ((status == 409 || status == 400) && (code.startsWith(QLatin1String("application_")) || code == QLatin1String("bad_answers"))) {
            emit application(link, profile, code, json.object().value("pond").toObject().toVariantMap());
            return;
        }
        if (status != 201) {
            emit failed(why(status, json, tr("join that Pond")));
            return;
        }
        const QString id = json.object().value("id").toString();
        version_ = -1;
        refresh();
        open(id);
        emit joined(id);
    });
}

void Ponds::open(const QString &pondId)
{
    if (pondId == current_)
        return;
    current_ = pondId;
    channel_.clear();
    info_ = QJsonObject();
    for (const QJsonObject &p : list_.items())
        if (p.value("id").toString() == pondId)
            info_ = p;
    loaded_.clear();
    messages_.clear();
    logLoaded_.clear();
    log_.clear();
    busyLog_ = false;
    channels_.clear();
    bans_.clear();
    invites_.clear();
    members_.clear();
    pages_.clear();
    closePage();
    generation_++;
    emit currentChanged();
    emit currentInfoChanged();
    emit channelChanged();
    if (!pondId.isEmpty())
        refreshPond();
}

void Ponds::openChannel(const QString &channelId)
{
    closePage();
    if (channelId == channel_)
        return;
    channel_ = channelId;
    if (!current_.isEmpty() && !channelId.isEmpty())
        lastChannel_.insert(current_, channelId);
    loaded_.clear();
    messages_.clear();
    logLoaded_.clear();
    log_.clear();
    busyLog_ = false;
    generation_++;
    busyMessages_ = busyOlder_ = false;
    if (hasOlder_) {
        hasOlder_ = false;
        emit hasOlderChanged();
    }
    emit channelChanged();
    refreshMessages();
}

void Ponds::close()
{
    open(QString());
}

// ---- pages ----

void Ponds::openPage(const QString &pageId)
{
    if (pageId == page_)
        return;
    page_ = pageId;
    pageData_.clear();
    pageGen_++;
    emit pageChanged();
    emit pageDataChanged();
    loadPage();
}

void Ponds::closePage()
{
    if (page_.isEmpty())
        return;
    page_.clear();
    pageData_.clear();
    pageGen_++;
    emit pageChanged();
    emit pageDataChanged();
}

void Ponds::loadPage()
{
    if (page_.isEmpty() || current_.isEmpty())
        return;
    const int gen = pageGen_;
    api_->get(QStringLiteral("/api/v1/ponds/%1/pages/%2").arg(current_, page_), [this, gen](int status, const QJsonDocument &json) {
        if (gen != pageGen_)
            return;
        if (status != 200) {
            if (status == 404)
                closePage();
            return;
        }
        QVariantMap d = json.object().toVariantMap();
        d.insert("id", QString::number(qint64(json.object().value("id").toDouble())));
        d.insert("markdown", pageMarkdown(d.value("body").toString()));
        d.insert("updatedBy", nameOf(d.value("updated_by").toString()));
        pageData_ = d;
        emit pageDataChanged();
    });
}

QString Ponds::pageMarkdown(const QString &body) const
{
    QHash<QString, QString> ids;
    for (const QJsonObject &c : channels_.items())
        ids.insert(c.value("name").toString(), c.value("id").toString());
    // a channel's name after # where a word can't go on: not a heading ("# Title"), not inside a word or a link
    static const QRegularExpression name(QStringLiteral("(^|[\\s(,;:!?])#([a-z0-9-]{1,32})(?![a-z0-9-])"));
    QStringList out;
    bool fence = false;
    for (const QString &line : body.split(u'\n')) {
        if (line.trimmed().startsWith(QLatin1String("```"))) {
            fence = !fence;
            out << line;
            continue;
        }
        if (fence) {
            out << line;
            continue;
        }
        // between backticks it stays as written
        const QStringList parts = line.split(u'`');
        QStringList done;
        for (int i = 0; i < parts.size(); i++) {
            QString part = parts.at(i);
            if (i % 2 == 0) {
                QString linked;
                qsizetype at = 0;
                auto it = name.globalMatch(part);
                while (it.hasNext()) {
                    const QRegularExpressionMatch m = it.next();
                    const QString id = ids.value(m.captured(2));
                    linked += part.mid(at, m.capturedStart(2) - 1 - at);
                    linked += id.isEmpty() ? QStringLiteral("#") + m.captured(2) : QStringLiteral("[#%1](channel:%2)").arg(m.captured(2), id);
                    at = m.capturedEnd(2);
                }
                part = linked + part.mid(at);
            }
            done << part;
        }
        out << done.join(u'`');
    }
    return out.join(u'\n');
}

void Ponds::createPage(const QString &title, const QString &kind)
{
    const QString id = current_, t = title.trimmed();
    if (id.isEmpty() || t.isEmpty())
        return;
    QJsonObject body{ { "title", t }, { "body", QString() } };
    if (kind != QLatin1String("markdown"))
        body.insert("kind", kind);
    api_->post(QStringLiteral("/api/v1/ponds/%1/admin/pages/create").arg(id), body,
               [this, id](int status, const QJsonDocument &json) {
                   if (status != 200) {
                       emit adminFailed(why(status, json, tr("make that page")));
                       return;
                   }
                   emit adminDone(tr("Page made."));
                   refreshPond();
                   if (id == current_)
                       openPage(QString::number(qint64(json.object().value("id").toDouble())));
               });
}

void Ponds::savePage(const QString &pageId, const QString &title, const QString &body, const QVariantList &files, const QVariantMap &extra)
{
    QJsonArray list;
    for (const QVariant &f : files) {
        const QVariantMap m = f.toMap();
        QJsonObject o{ { "id", m.value("id").toString() }, { "name", m.value("name").toString() }, { "mime", m.value("mime").toString() },
                       { "size", m.value("size").toDouble() } };
        if (m.value("w").toInt() > 0 && m.value("h").toInt() > 0) {
            o.insert("w", m.value("w").toInt());
            o.insert("h", m.value("h").toInt());
        }
        list.append(o);
    }
    const QString id = current_;
    api_->post(QStringLiteral("/api/v1/ponds/%1/admin/pages/%2/edit").arg(id, pageId),
               [&] {
                   QJsonObject o = QJsonObject::fromVariantMap(extra);
                   o.insert("title", title.trimmed());
                   o.insert("body", body);
                   o.insert("files", list);
                   return o;
               }(),
               [this, id](int status, const QJsonDocument &json) {
                   if (status < 200 || status > 299) {
                       emit adminFailed(json.object().value("message").toString().isEmpty() ? why(status, json, tr("save that page"))
                                                                                           : json.object().value("message").toString());
                       return;
                   }
                   emit adminDone(tr("Page saved."));
                   if (id == current_) {
                       emit pageSaved();
                       loadPage();
                       refreshPond();
                   }
               });
}

void Ponds::deletePage(const QString &pageId)
{
    admin(QStringLiteral("pages/%1/delete").arg(pageId), {}, tr("Page deleted."), tr("delete that page"));
}

void Ponds::movePage(const QString &pageId, int by)
{
    QStringList order;
    for (const QJsonObject &p : pages_.items())
        order << p.value("id").toString();
    const int from = int(order.indexOf(pageId)), to = from + by;
    if (from < 0 || to < 0 || to >= order.size())
        return;
    order.move(from, to);
    QJsonArray ids;
    for (const QString &p : order)
        ids.append(p.toLongLong());
    admin(QStringLiteral("pages/order"), { { "pages", ids } }, tr("Moved."), tr("move that page"));
}

void Ponds::fetchPageFile(const QString &fileId)
{
    if (page_.isEmpty() || current_.isEmpty())
        return;
    api_->post(QStringLiteral("/api/v1/ponds/%1/pages/%2/files/%3/download").arg(current_, page_, fileId), QJsonObject{},
               [this](int status, const QJsonDocument &json) {
                   if (status != 204 && status != 200)
                       emit failed(why(status, json, tr("fetch that file")));
                   else
                       loadPage();
               });
}

void Ponds::setRoleColorsEnforced(bool on)
{
    admin(QStringLiteral("role_colors"), QJsonObject{ { "enforced", on } },
          on ? tr("Everyone's name now shows in their role's colour.") : tr("Each member now chooses their name's colour."), tr("change that"));
}

void Ponds::setUseRoleColor(bool on)
{
    if (current_.isEmpty())
        return;
    api_->post(QStringLiteral("/api/v1/ponds/%1/role_color").arg(current_), QJsonObject{ { "use", on } }, [this](int status, const QJsonDocument &json) {
        if (status / 100 != 2)
            emit failed(why(status, json, tr("change your name's colour")));
        refreshPond();
    });
}

bool Ponds::useRoleColor() const
{
    return people_.value(me_).value("useRoleColor").toBool();
}

QString Ponds::myRoleColor() const
{
    for (const QVariant &r : rolesOf(me_)) /* highest first */
        if (!r.toMap().value("color").toString().isEmpty())
            return r.toMap().value("color").toString();
    return QString();
}

void Ponds::setBanner(const QString &hash)
{
    QJsonObject body;
    body.insert("hash", hash.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(hash));
    admin(QStringLiteral("banner"), body, hash.isEmpty() ? tr("Banner removed.") : tr("Banner set."), tr("change the banner"));
}

void Ponds::send(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || current_.isEmpty() || channel_.isEmpty())
        return;
    api_->post(pathOfChannel() + QStringLiteral("/send"), QJsonObject{ { "body", t } }, [this](int status, const QJsonDocument &json) {
        if (status != 200) {
            emit failed(why(status, json, tr("send that")));
            return;
        }
        refreshMessages();
    });
}

void Ponds::loadOlder()
{
    if (!hasOlder_ || busyOlder_ || loaded_.isEmpty())
        return;
    busyOlder_ = true;
    const int gen = generation_;
    const QString before = QString::number(qint64(loaded_.first().value("id").toDouble()));
    api_->get(pathOfChannel() + QStringLiteral("?before=") + before, [this, gen](int status, const QJsonDocument &json) {
        if (gen != generation_)
            return;
        busyOlder_ = false;
        if (status != 200) {
            if (status != 0)
                emit failed(why(status, json, tr("load earlier messages")));
            return;
        }
        const QJsonObject o = json.object();
        QList<QJsonObject> page;
        for (const QJsonValue &m : o.value("items").toArray())
            page.append(m.toObject());
        loaded_ = page + loaded_;
        const bool older = o.value("more").toBool();
        if (older != hasOlder_) {
            hasOlder_ = older;
            emit hasOlderChanged();
        }
        rebuildMessages();
    });
}

void Ponds::leave(const QString &pondId)
{
    api_->post(QStringLiteral("/api/v1/ponds/%1/leave").arg(pondId), {}, [this, pondId](int status, const QJsonDocument &json) {
        if (status != 204) {
            emit failed(why(status, json, tr("leave that Pond")));
            return;
        }
        if (pondId == current_)
            close();
        version_ = -1;
        refresh();
    });
}

void Ponds::setProfile(const QString &pondId, const QString &profile)
{
    QJsonObject body;
    body.insert("profile", profile.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(profile));
    api_->post(QStringLiteral("/api/v1/ponds/%1/profile").arg(pondId), body, [this](int status, const QJsonDocument &json) {
        if (status != 200)
            emit failed(why(status, json, tr("change how you appear there")));
    });
}

// ---- voice channels ----

void Ponds::takeVoice(const QJsonObject &voice)
{
    if (voice == voice_)
        return;
    voice_ = voice;
    voicePeople_ = voiceIn_.value(voicePond()).value(voiceChannel());
    emit voiceChanged();
}

// who is in our voice channel, as the Pond last said (slots are given in turn, so by slot is about the order they came)
void Ponds::updateVoicePeople()
{
    const QStringList now = voiceIn_.value(voicePond()).value(voiceChannel());
    if (now == voicePeople_)
        return;
    voicePeople_ = now;
    emit voiceChanged();
}

void Ponds::refreshVoice()
{
    if (busyVoice_ || !api_->ready())
        return;
    busyVoice_ = true;
    const int gen = voiceGen_;
    api_->get(QStringLiteral("/api/v1/voice"), [this, gen](int status, const QJsonDocument &json) {
        busyVoice_ = false;
        if (status == 200 && gen == voiceGen_)
            takeVoice(json.object().value("voice").toObject());
    });
}

void Ponds::joinVoice(const QString &channelId)
{
    if (current_.isEmpty() || channelId.isEmpty())
        return;
    if (voicePond() == current_ && voiceChannel() == channelId && voiceState() != QLatin1String("ended"))
        return; /* there already */
    ++voiceGen_;
    api_->post(QStringLiteral("/api/v1/ponds/%1/channels/%2/voice/join").arg(current_, channelId), QJsonObject(),
               [this](int status, const QJsonDocument &json) {
                   if (status == 200)
                       takeVoice(json.object().value("voice").toObject());
                   else
                       emit failed(why(status, json, tr("join the voice channel")));
               });
}

void Ponds::leaveVoice()
{
    ++voiceGen_;
    takeVoice(QJsonObject()); /* at once: the Puddle lets it go whatever happens */
    api_->post(QStringLiteral("/api/v1/voice/leave"), QJsonObject(), [this](int, const QJsonDocument &) { refreshVoice(); });
}

// Muted, deafened, camera or screen: shown at once; the Puddle's answer has the last word
void Ponds::setVoiceFlag(const QString &name, bool on)
{
    if (voice_.isEmpty())
        return;
    voice_.insert(name, on);
    emit voiceChanged();
    const int gen = ++voiceGen_;
    api_->post(QStringLiteral("/api/v1/voice/state"), QJsonObject{ { name, on } }, [this, gen](int status, const QJsonDocument &json) {
        if (gen != voiceGen_)
            return;
        if (status == 200)
            takeVoice(json.object().value("voice").toObject());
        else
            refreshVoice();
    });
}

void Ponds::setVoiceMuted(bool muted) { setVoiceFlag(QStringLiteral("muted"), muted); }
void Ponds::setVoiceDeafened(bool deafened) { setVoiceFlag(QStringLiteral("deafened"), deafened); }
void Ponds::setVoiceCamera(bool on) { setVoiceFlag(QStringLiteral("camera"), on); }
void Ponds::setVoiceScreen(bool on) { setVoiceFlag(QStringLiteral("screen"), on); }

// ---- protocol 13: page kinds, the shop, applications, permissions, voice moderation ----

void Ponds::post(const QString &path, const QJsonObject &body, const QString &done, const QString &doing, std::function<void()> then)
{
    if (current_.isEmpty())
        return;
    const QString id = current_;
    api_->post(QStringLiteral("/api/v1/ponds/%1/%2").arg(id, path), body, [this, id, done, doing, then](int status, const QJsonDocument &json) {
        if (status < 200 || status > 299) {
            const QJsonObject o = json.object();
            const QString code = o.value("error").toString();
            emit adminFailed(!o.value("message").toString().isEmpty()      ? o.value("message").toString()
                             : code == QLatin1String("outranked")          ? tr("That is above your highest role, or more than you may do.")
                             : code == QLatin1String("not_offered")        ? tr("That page doesn't offer that role.")
                             : code == QLatin1String("one_only")           ? tr("Pick one of these roles.")
                             : code == QLatin1String("no_payments")        ? tr("This shop doesn't take payments yet.")
                             : code == QLatin1String("sold_out")           ? tr("There aren't that many left.")
                             : code == QLatin1String("too_little")         ? tr("Pay at least %1.").arg(o.value("least").toDouble() / 100.0, 0, 'f', 2)
                             : code == QLatin1String("no_permission")      ? tr("You may not do that there.")
                             : code == QLatin1String("not_yourself")       ? tr("You can't do that to yourself.")
                             : code == QLatin1String("no_session")         ? tr("They aren't in a voice channel now.")
                             : code == QLatin1String("unknown_permission") ? tr("This Pond doesn't know that permission.")
                                                                           : why(status, json, doing));
            return;
        }
        if (!done.isEmpty())
            emit adminDone(done);
        if (id != current_)
            return;
        if (then)
            then();
    });
}

void Ponds::pickRoles(const QString &pageId, const QStringList &roleIds)
{
    QJsonArray ids;
    for (const QString &r : roleIds)
        ids.append(r.toLongLong());
    post(QStringLiteral("pages/%1/roles").arg(pageId), { { "roles", ids } }, tr("Your roles are changed."), tr("change your roles"), [this] {
        loadPage();
        refreshPond();
    });
}

void Ponds::setGoing(const QString &pageId, const QString &eventId, bool going)
{
    post(QStringLiteral("pages/%1/events/%2/going").arg(pageId, eventId), { { "going", going } }, QString(), tr("say whether you're going"),
         [this] { loadPage(); });
}

void Ponds::saveEvent(const QString &pageId, const QString &eventId, const QVariantMap &fields)
{
    const QString path = eventId.isEmpty() ? QStringLiteral("admin/pages/%1/events/create").arg(pageId)
                                           : QStringLiteral("admin/pages/%1/events/%2/edit").arg(pageId, eventId);
    post(path, QJsonObject::fromVariantMap(fields), eventId.isEmpty() ? tr("Event added.") : tr("Event saved."), tr("save that event"),
         [this] { loadPage(); });
}

void Ponds::deleteEvent(const QString &pageId, const QString &eventId)
{
    post(QStringLiteral("admin/pages/%1/events/%2/delete").arg(pageId, eventId), {}, tr("Event deleted."), tr("delete that event"), [this] { loadPage(); });
}

void Ponds::saveItem(const QString &pageId, const QString &itemId, const QVariantMap &fields)
{
    const QString path = itemId.isEmpty() ? QStringLiteral("admin/pages/%1/items/create").arg(pageId)
                                          : QStringLiteral("admin/pages/%1/items/%2/edit").arg(pageId, itemId);
    post(path, QJsonObject::fromVariantMap(fields), itemId.isEmpty() ? tr("Item added.") : tr("Item saved."), tr("save that item"), [this] { loadPage(); });
}

void Ponds::deleteItem(const QString &pageId, const QString &itemId)
{
    post(QStringLiteral("admin/pages/%1/items/%2/delete").arg(pageId, itemId), {}, tr("Item taken off."), tr("take that item off"), [this] { loadPage(); });
}

void Ponds::buy(const QString &pageId, const QString &itemId, int quantity, double amount)
{
    if (current_.isEmpty())
        return;
    QJsonObject body{ { "quantity", quantity } };
    if (amount > 0)
        body.insert("amount", qint64(amount));
    const QString id = current_;
    api_->post(QStringLiteral("/api/v1/ponds/%1/pages/%2/items/%3/buy").arg(id, pageId, itemId), body, [this](int status, const QJsonDocument &json) {
        const QJsonObject o = json.object();
        if (status != 200 || !o.value("url").toString().startsWith(QLatin1String("https://"))) {
            const QString code = o.value("error").toString();
            emit failed(!o.value("message").toString().isEmpty() ? o.value("message").toString()
                        : code == QLatin1String("no_payments")   ? tr("This shop doesn't take payments yet.")
                        : code == QLatin1String("sold_out")      ? tr("There aren't that many left.")
                        : code == QLatin1String("too_little")    ? tr("Pay at least %1.").arg(o.value("least").toDouble() / 100.0, 0, 'f', 2)
                                                                 : why(status, json, tr("start that order")));
            return;
        }
        emit checkout(o.value("url").toString());
        loadOrders(false);
    });
}

void Ponds::loadOrders(bool all)
{
    if (current_.isEmpty())
        return;
    const QString id = current_;
    api_->get(QStringLiteral("/api/v1/ponds/%1/%2").arg(id, all ? QStringLiteral("admin/orders") : QStringLiteral("orders")),
              [this, id](int status, const QJsonDocument &json) {
                  if (status != 200 || id != current_)
                      return;
                  QList<QJsonObject> rows;
                  for (const QJsonValue &v : json.object().value("orders").toArray()) {
                      QJsonObject o = v.toObject();
                      o.insert("id", QString::number(qint64(o.value("id").toDouble())));
                      o.insert("item", QString::number(qint64(o.value("item").toDouble())));
                      o.insert("memberName", nameOf(o.value("member").toString()));
                      rows << o;
                  }
                  orders_.setItems(rows);
              });
}

void Ponds::setStripeKey(const QString &key)
{
    const QString k = key.trimmed();
    admin(QStringLiteral("shop/stripe"), { { "key", k.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(k) } },
          k.isEmpty() ? tr("The shop no longer takes payments.") : tr("Stripe is connected: the shop takes payments."), tr("connect Stripe"));
}

void Ponds::loadApplications()
{
    if (current_.isEmpty() || !canManageApplications())
        return;
    const QString id = current_;
    api_->get(QStringLiteral("/api/v1/ponds/%1/admin/applications").arg(id), [this, id](int status, const QJsonDocument &json) {
        if (status != 200 || id != current_)
            return;
        QList<QJsonObject> rows;
        int waiting = 0;
        for (const QJsonValue &v : json.object().value("applications").toArray()) {
            QJsonObject o = v.toObject();
            const QString name = cardOf(o.value("profile").toObject()).value("name").toString();
            o.insert("name", name.isEmpty() ? o.value("member").toString().left(8) : name);
            o.remove("profile");
            if (o.value("status").toString() == QLatin1String("pending"))
                waiting++;
            rows << o;
        }
        applications_.setItems(rows);
        if (waiting != applicationsWaiting_) {
            applicationsWaiting_ = waiting;
            emit applicationsChanged();
        }
    });
}

void Ponds::decideApplication(const QString &memberId, bool accept, const QString &reason)
{
    QJsonObject body;
    if (!accept && !reason.trimmed().isEmpty())
        body.insert("reason", reason.trimmed());
    post(QStringLiteral("admin/applications/%1/%2").arg(memberId, accept ? QStringLiteral("accept") : QStringLiteral("decline")), body,
         accept ? tr("Accepted: they are a member now.") : tr("Declined."), tr("answer that application"), [this] {
             loadApplications();
             refreshPond();
         });
}

void Ponds::setEveryone(const QStringList &permissions)
{
    admin(QStringLiteral("everyone"), { { "permissions", QJsonArray::fromStringList(permissions) } }, tr("Saved what everyone may do."),
          tr("change what everyone may do"));
}

void Ponds::setChannelOverrides(const QString &channelId, const QVariantList &overrides)
{
    QJsonArray list;
    for (const QVariant &v : overrides) {
        const QVariantMap m = v.toMap();
        const QStringList allow = m.value("allow").toStringList(), deny = m.value("deny").toStringList();
        if (allow.isEmpty() && deny.isEmpty())
            continue;
        list.append(QJsonObject{ { "role", m.value("role").toString().toLongLong() }, { "allow", QJsonArray::fromStringList(allow) },
                                 { "deny", QJsonArray::fromStringList(deny) } });
    }
    admin(QStringLiteral("channels/%1/overrides").arg(channelId), { { "overrides", list } }, tr("Channel permissions saved."),
          tr("change that channel's permissions"));
}

void Ponds::voiceMute(const QString &memberId, bool muted)
{
    post(QStringLiteral("voice/members/%1/mute").arg(memberId), { { "muted", muted } },
         muted ? tr("Muted in voice, for everyone.") : tr("Unmuted in voice."), tr("mute them in voice"), [this] { refreshPond(); });
}

void Ponds::voiceDisconnect(const QString &memberId)
{
    post(QStringLiteral("voice/members/%1/disconnect").arg(memberId), QJsonObject(), tr("Disconnected from voice."), tr("disconnect them"),
         [this] { refreshPond(); });
}

QVariantMap Ponds::inVoice(const QString &memberId) const
{
    for (const QJsonObject &c : channels_.items())
        for (const QJsonValue &v : c.value("voice").toArray())
            if (v.toObject().value("id").toString() == memberId)
                return { { "channel", c.value("id").toString() }, { "serverMuted", v.toObject().value("serverMuted").toBool() } };
    return {};
}
