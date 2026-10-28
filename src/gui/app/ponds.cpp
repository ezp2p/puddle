#include "ponds.h"

#include <QDateTime>
#include <QJsonArray>
#include <QUrl>

Ponds::Ponds(ApiClient *api, QObject *parent)
    : QObject(parent),
      api_(api),
      list_({ "id", "name", "status", "address" }),
      channels_({ "id", "name", "topic" }),
      members_({ "id", "name", "color", "isMe" }),
      messages_({ "id", "author", "name", "tint", "text", "ts", "mine", "grouped", "dayStart" })
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
    if (code == QLatin1String("slow_down"))
        return tr("You're sending messages too quickly. Wait a moment.");
    if (code == QLatin1String("certificate_changed"))
        return tr("The Pond's certificate changed, so nothing was sent.");
    return tr("Couldn't %1.").arg(doing);
}

// A channel id comes as a number; the window keeps them as text, like every other id.
static QJsonObject channelRow(const QJsonObject &c)
{
    return { { "id", QString::number(qint64(c.value("id").toDouble())) }, { "name", c.value("name") }, { "topic", c.value("topic") } };
}

void Ponds::refresh()
{
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
            rows.append({ { "id", po.value("id") }, { "name", po.value("name") }, { "status", po.value("status") }, { "address", po.value("address") } });
            if (po.value("id").toString() == current_)
                stillThere = true;
        }
        list_.setItems(rows);
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
            people_.insert(mid, { { "name", name }, { "color", color } });
            members.append({ { "id", mid }, { "name", name }, { "color", color }, { "isMe", mid == me_ } });
        }
        std::stable_sort(members.begin(), members.end(), [](const QJsonObject &a, const QJsonObject &b) {
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
                       { "name", mine ? tr("You") : who.isEmpty() ? author.left(8) : who.value("name").toString() },
                       { "tint", who.value("color").toString() },
                       { "text", m.value("text") },
                       { "ts", double(ts) },
                       { "mine", mine },
                       { "dayStart", newDay },
                       { "grouped", !first && !newDay && prev.value("author").toString() == author && ts - qint64(prev.value("ts").toDouble()) < 7 * 60 * 1000 } };
        rows.append(r);
        prev = m;
    }
    messages_.setItems(rows);
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
