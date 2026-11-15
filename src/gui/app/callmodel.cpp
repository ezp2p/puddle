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
        const QJsonArray items = o.value("items").toArray();
        take(items.isEmpty() ? QJsonObject() : items.first().toObject());
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
    incoming_ = live && call.value("direction").toString() == QLatin1String("incoming");
    answered_ = live ? qint64(call.value("answered").toDouble()) : 0;
    if (!live && muted_) {
        muted_ = false;
        emit mutedChanged();
    }
    if (state_ != was || id_ != wasId) {
        emit changed();
        if (state_ == QLatin1String("ringing") && (was != QLatin1String("ringing") || id_ != wasId))
            emit ringing(user_);
    }
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
    return QObject::tr("Couldn't do that (%1).").arg(status);
}

void Calls::call(const QString &user)
{
    setError(QString());
    api_->post(QStringLiteral("/api/v1/calls/start"), { { "user", user } }, [this](int status, const QJsonDocument &json) {
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

QString Calls::describe(const QString &reason, bool incoming, const QString &name, qint64 seconds) const
{
    const QString length = seconds >= 3600 ? QStringLiteral("%1:%2:%3").arg(seconds / 3600).arg(seconds / 60 % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'))
                                            : QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
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
