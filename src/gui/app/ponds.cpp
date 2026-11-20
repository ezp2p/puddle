#include "ponds.h"

#include <QDateTime>

#include <algorithm>
#include <QJsonArray>
#include <QUrl>

Ponds::Ponds(ApiClient *api, QObject *parent)
    : QObject(parent),
      api_(api),
      list_({ "id", "name", "status", "address", "icon" }),
      channels_({ "id", "name", "topic", "kind", "voice" }),
      members_({ "id", "name", "color", "avatar", "isMe", "role", "muted", "mutedUntil" }),
      messages_({ "id", "author", "name", "tint", "avatar", "text", "files", "ts", "mine", "grouped", "dayStart" }),
      bans_({ "member", "name", "by", "byName", "reason", "ts" }, nullptr, QStringLiteral("member")),
      invites_({ "token", "link", "uses", "maxUses", "expires", "created" }, nullptr, QStringLiteral("token"))
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
QJsonObject Ponds::channelRow(const QJsonObject &c) const
{
    QJsonArray in;
    for (const QJsonValue &v : c.value("voice").toArray()) {
        const QJsonObject vo = v.toObject();
        const QString mid = vo.value("member").toString();
        const QJsonObject who = people_.value(mid);
        in.append(QJsonObject{ { "id", mid }, { "name", who.value("name").toString(mid.left(8)) }, { "color", who.value("color") },
                               { "avatar", who.value("avatar").toString() },
                               { "muted", vo.value("muted") }, { "deafened", vo.value("deafened") }, { "isMe", mid == me_ } });
    }
    return { { "id", QString::number(qint64(c.value("id").toDouble())) }, { "name", c.value("name") }, { "topic", c.value("topic") },
             { "kind", c.value("kind").toString(QStringLiteral("text")) }, { "voice", in } };
}

void Ponds::refresh()
{
    refreshVoice();
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
            const QJsonArray colors = prof.value("color").toObject().value("colors").toArray();
            const QString color = colors.isEmpty() ? QString() : colors.first().toString();
            const QString avatar = prof.value("avatar").toObject().value("hash").toString();
            // muted by an operator or administrator, and not run out: until when (0: until unmuted)
            const QJsonValue mu = mo.value("muted");
            const double until = mu.isObject() ? mu.toObject().value("until").toDouble(0) : -1;
            const bool muted = mu.isObject() && (until == 0 || until > double(QDateTime::currentMSecsSinceEpoch()));
            people_.insert(mid, { { "name", name }, { "color", color }, { "avatar", avatar }, { "profile", prof }, { "role", mo.value("role").toString(QStringLiteral("member")) },
                                  { "muted", muted }, { "mutedUntil", muted ? until : -1 } });
            members.append({ { "id", mid }, { "name", name }, { "color", color }, { "avatar", avatar }, { "isMe", mid == me_ },
                             { "muted", muted }, { "mutedUntil", muted ? until : -1 },
                             { "role", mo.value("role").toString(QStringLiteral("member")) } });
        }
        // operators first, then administrators, then everyone else; by name within each
        auto rank = [](const QJsonObject &m) { const QString r = m.value("role").toString(); return r == "operator" ? 0 : r == "administrator" ? 1 : 2; };
        std::stable_sort(members.begin(), members.end(), [&](const QJsonObject &a, const QJsonObject &b) {
            if (rank(a) != rank(b))
                return rank(a) < rank(b);
            return a.value("name").toString().compare(b.value("name").toString(), Qt::CaseInsensitive) < 0;
        });
        members_.setItems(members);
        QList<QJsonObject> channels;
        for (const QJsonValue &c : o.value("channels").toArray())
            channels.append(channelRow(c.toObject()));
        channels_.setItems(channels);
        info_ = o;
        emit currentInfoChanged();
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
        if (loaded_.isEmpty() || oldestOfPage > newest) {
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
        const QJsonObject who = people_.value(author);
        const bool first = prev.isEmpty();
        const bool newDay = first || QDateTime::fromMSecsSinceEpoch(ts).date() != QDateTime::fromMSecsSinceEpoch(qint64(prev.value("ts").toDouble())).date();
        const bool mine = author == me_;
        QJsonObject r{ { "id", QString::number(qint64(m.value("id").toDouble())) },
                       { "author", author },
                       { "name", !who.value("name").toString().isEmpty() ? who.value("name").toString() : mine ? tr("You") : author.left(8) },
                       { "tint", who.value("color").toString() },
                       { "avatar", who.value("avatar").toString() },
                       { "text", m.value("text") },
                       { "files", m.value("files") },
                       { "ts", double(ts) },
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
                             : code == QLatin1String("no_role")       ? tr("You no longer have a role in this Pond.")
                             : code == QLatin1String("outranked")     ? tr("They are your rank or above.")
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

QVariantMap Ponds::cardOf(const QJsonObject &profile)
{
    QVariantList colors;
    for (const QJsonValue &c : profile.value("color").toObject().value("colors").toArray())
        colors << c.toString();
    return { { "name", profile.value("display_name").toString() }, { "avatar", profile.value("avatar").toObject().value("hash").toString() },
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
    return c;
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
            emit adminFailed(code == QLatin1String("no_role") ? tr("You no longer have a role in this Pond.") : why(status, json, tr("make an invite")));
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
    QString theirs = QStringLiteral("member");
    for (const QJsonObject &m : members_.items())
        if (m.value("id").toString() == memberId)
            theirs = m.value("role").toString();
    return memberId != me_ && theirs != QLatin1String("operator") && rank(currentRole()) > rank(theirs);
}

void Ponds::join(const QString &link, const QString &profile)
{
    if (joining_)
        return;
    joining_ = true;
    emit joiningChanged();
    QJsonObject body{ { "link", link.trimmed() } };
    body.insert("profile", profile.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(profile));
    api_->post(QStringLiteral("/api/v1/ponds/join"), body, [this](int status, const QJsonDocument &json) {
        joining_ = false;
        emit joiningChanged();
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
    channels_.clear();
    bans_.clear();
    invites_.clear();
    members_.clear();
    generation_++;
    emit currentChanged();
    emit currentInfoChanged();
    emit channelChanged();
    if (!pondId.isEmpty())
        refreshPond();
}

void Ponds::openChannel(const QString &channelId)
{
    if (channelId == channel_)
        return;
    channel_ = channelId;
    if (!current_.isEmpty() && !channelId.isEmpty())
        lastChannel_.insert(current_, channelId);
    loaded_.clear();
    messages_.clear();
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
