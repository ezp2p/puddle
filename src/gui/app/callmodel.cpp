#include "callmodel.h"

#include <QJsonArray>
#include <QJsonObject>

Calls::Calls(ApiClient *api, QObject *parent) : QObject(parent), api_(api)
{
    timer_.setInterval(700);
    connect(&timer_, &QTimer::timeout, this, &Calls::refresh);
}

void Calls::refresh()
{
    if (busy_ || !api_->ready())
        return;
    busy_ = true;
    api_->get(QStringLiteral("/api/v1/calls?limit=1"), [this](int status, const QJsonDocument &json) {
        busy_ = false;
        if (status != 200)
            return;
        const QJsonObject o = json.object();
        const quint64 v = quint64(o.value("version").toDouble());
        if (v == version_)
            return;
        version_ = v;
        // the first is the call going on, if any; then the group calls we are not in (rung by one, or open to join)
        QJsonObject first;
        QHash<QString, int> groups;
        for (const QJsonValue &item : o.value("items").toArray()) {
            const QJsonObject c = item.toObject();
            const QString state = c.value("state").toString();
            if (state == QLatin1String("ended"))
                break; /* the history, after what is going on */
            if (!c.value("group").toString().isEmpty())
                groups.insert(c.value("group").toString(), c.value("members").toArray().size() + (state == QLatin1String("open") || state == QLatin1String("ringing") ? 0 : 1));
            if (first.isEmpty() && state != QLatin1String("open"))
                first = c;
        }
        take(first);
        if (groups != groupCalls_) {
            groupCalls_ = groups;
            emit groupCallsChanged();
        }
        emit historyChanged();
    });
}

void Calls::take(const QJsonObject &call)
{
    const QString state = call.value("state").toString();
    const bool live = !call.isEmpty() && state != QLatin1String("ended");
    const QString was = state_, wasId = id_;
    state_ = live ? state : QString();
    id_ = live ? call.value("id").toString() : QString();
    user_ = live ? call.value("user").toString() : QString();
    const QString wasGroup = group_;
    const int wasOthers = others_;
    group_ = live ? call.value("group").toString() : QString();
    others_ = live ? int(call.value("members").toArray().size()) : 0;
    incoming_ = live && call.value("direction").toString() == QLatin1String("incoming");
    answered_ = live ? qint64(call.value("answered").toDouble()) : 0;
    const QJsonObject dev = call.value("device").toObject();
    const QString wasDevice = device_;
    device_ = live ? dev.value("id").toString() : QString();
    deviceName_ = live ? dev.value("name").toString() : QString();
    const bool wasHere = here_;
    here_ = live && device_ == myDevice_;
    if (!live && muted_) {
        muted_ = false;
        emit mutedChanged();
    }
    followLeg();
    if (state_ != was || id_ != wasId || device_ != wasDevice || here_ != wasHere || group_ != wasGroup || others_ != wasOthers) {
        emit changed();
        if (state_ == QLatin1String("ringing") && (was != QLatin1String("ringing") || id_ != wasId))
            emit ringing(user_, group_);
    }
}

void Calls::setDevice(const QString &id, const QString &host)
{
    myDevice_ = id;
    host_ = host;
    if (!leg_)
        leg_ = new CallLeg(this);
}

QString Calls::elsewhere() const
{
    if (state_.isEmpty() || here_ || state_ == QLatin1String("ringing"))
        return QString();
    if (device_.isEmpty())
        return tr("on the computer");
    return deviceName_.isEmpty() ? tr("on a phone") : tr("on %1").arg(deviceName_);
}

// A phone: the call it has needs a leg, set up once (again if the call changed); any other call, or none, needs none.
void Calls::followLeg()
{
    if (!leg_)
        return;
    const bool want = here_ && (state_ == QLatin1String("calling") || state_ == QLatin1String("connecting") || state_ == QLatin1String("active"));
    if (!want) {
        if (!leg_->callId().isEmpty())
            leg_->close();
        return;
    }
    if (leg_->callId() == id_)
        return;
    const QString id = id_;
    leg_->open(id, [this, id](const QString &sdp) {
        if (id != id_)
            return;
        if (sdp.isEmpty()) {
            setError(CallLeg::built() ? tr("Couldn't set up the sound for this call.") : tr("This phone was built without call sound."));
            return;
        }
        api_->post(QStringLiteral("/api/v1/calls/") + id + QStringLiteral("/media"), { { "sdp", sdp } },
                   [this, id](int status, const QJsonDocument &json) {
                       if (id != id_ || leg_->callId() != id)
                           return;
                       const QJsonObject o = json.object();
                       if (status != 200 || !leg_->answer(o.value("sdp").toString(), host_, o.value("udp").toInt()))
                           setError(tr("Couldn't set up the sound for this call."));
                   });
    });
}

void Calls::setError(const QString &e)
{
    if (e == error_)
        return;
    error_ = e;
    emit errorChanged();
}

static QString words(int status, const QJsonDocument &json)
{
    const QString e = json.object().value("error").toString();
    if (e == QLatin1String("busy"))
        return QObject::tr("You're already in a call.");
    if (e == QLatin1String("no_stream"))
        return QObject::tr("You can only call friends.");
    if (e == QLatin1String("no_calls"))
        return QObject::tr("Calls are off on this Puddle.");
    if (e == QLatin1String("no_call"))
        return QObject::tr("That call is over.");
    if (e == QLatin1String("no_group"))
        return QObject::tr("You're not in that group.");
    if (e == QLatin1String("full"))
        return QObject::tr("That call is full.");
    return QObject::tr("Couldn't do that (%1).").arg(status);
}

void Calls::call(const QString &user) { start({ { "user", user } }); }
void Calls::callGroup(const QString &group) { start({ { "group", group } }); }

void Calls::start(const QJsonObject &body)
{
    setError(QString());
    api_->post(QStringLiteral("/api/v1/calls/start"), body, [this](int status, const QJsonDocument &json) {
        if (status == 201)
            take(json.object());
        else
            setError(words(status, json));
        refresh();
    });
}

void Calls::act(const QString &verb)
{
    if (id_.isEmpty())
        return;
    setError(QString());
    api_->post(QStringLiteral("/api/v1/calls/") + id_ + QLatin1Char('/') + verb, {}, [this](int status, const QJsonDocument &json) {
        if (status == 200)
            take(json.object());
        else
            setError(words(status, json));
        refresh();
    });
}

void Calls::accept() { act(QStringLiteral("accept")); }
void Calls::decline() { act(QStringLiteral("decline")); }
void Calls::hangUp() { act(QStringLiteral("end")); }

void Calls::setMuted(bool muted)
{
    if (muted == muted_)
        return;
    muted_ = muted;
    emit mutedChanged();
}

static QString lengthOf(qint64 seconds)
{
    return seconds >= 3600 ? QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'))
                           : QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString Calls::describeGroup(const QString &reason, bool incoming, const QString &name, qint64 seconds) const
{
    if (reason == QLatin1String("hung_up") || (reason == QLatin1String("failed") && seconds > 0))
        return incoming ? tr("%1 started a call · %2").arg(name, lengthOf(seconds)) : tr("You started a call · %1").arg(lengthOf(seconds));
    if (reason == QLatin1String("missed"))
        return tr("You missed a call %1 started").arg(name);
    if (reason == QLatin1String("declined"))
        return tr("You didn't join a call %1 started").arg(name);
    if (reason == QLatin1String("no_answer"))
        return tr("You started a call; nobody joined");
    if (reason == QLatin1String("cancelled"))
        return tr("You started a call, then left");
    return incoming ? tr("A call %1 started didn't connect").arg(name) : tr("Your call didn't connect");
}

QString Calls::describe(const QString &reason, bool incoming, const QString &name, qint64 seconds) const
{
    const QString length = lengthOf(seconds);
    if (reason == QLatin1String("hung_up") || (reason == QLatin1String("failed") && seconds > 0))
        return incoming ? tr("%1 called you · %2").arg(name, length) : tr("You called %1 · %2").arg(name, length);
    if (reason == QLatin1String("missed"))
        return tr("Missed call from %1").arg(name);
    if (reason == QLatin1String("declined"))
        return incoming ? tr("You declined a call from %1").arg(name) : tr("%1 declined your call").arg(name);
    if (reason == QLatin1String("cancelled"))
        return incoming ? tr("Missed call from %1").arg(name) : tr("You called %1, then hung up").arg(name);
    if (reason == QLatin1String("no_answer"))
        return incoming ? tr("Missed call from %1").arg(name) : tr("%1 didn't answer").arg(name);
    if (reason == QLatin1String("busy"))
        return incoming ? tr("%1 called while you were in a call").arg(name) : tr("%1 was in another call").arg(name);
    if (reason == QLatin1String("no_calls"))
        return tr("%1's Puddle doesn't take calls").arg(name);
    return incoming ? tr("A call from %1 didn't connect").arg(name) : tr("Your call to %1 didn't connect").arg(name);
}
