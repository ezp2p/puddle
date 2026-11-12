#include "phone.h"

#include "chats.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>

Phone::Phone(Chats *chats, const QString &dir, QObject *parent) : QObject(parent), chats_(chats), dir_(dir)
{
    poll_.setInterval(3000);
    connect(&poll_, &QTimer::timeout, this, &Phone::poll);
    /* the Puddle rolled its keys and proved the new one: keep it, from whichever client heard */
    auto keep = [this](const QByteArray &key) {
        key_ = key;
        save();
    };
    connect(&api_, &ApiClient::puddleKeyChanged, this, keep);
    connect(chats_->api(), &ApiClient::puddleKeyChanged, this, keep);
    load();
}

QString Phone::defaultName() const
{
    const QString n = QSysInfo::machineHostName();
    return n.isEmpty() || n == QLatin1String("localhost") ? QSysInfo::prettyProductName() : n;
}

void Phone::set(const QString &state, const QString &error)
{
    state_ = state;
    error_ = error;
    emit changed();
}

void Phone::load()
{
    QFile f(dir_ + QStringLiteral("/device.json"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    base_ = QUrl(o.value("base").toString());
    puddleId_ = o.value("puddle").toString();
    key_ = QByteArray::fromHex(o.value("puddle_key").toString().toLatin1());
    seed_ = QByteArray::fromHex(o.value("seed").toString().toLatin1());
    if (!base_.isValid() || puddleId_.isEmpty() || key_.size() != 32 || seed_.size() != 32) {
        error_ = tr("What this phone knew about its Puddle was damaged. Pair it again.");
        return;
    }
    check_ = ApiClient::checkOf(seed_);
    if (o.value("state").toString() == QLatin1String("active")) {
        attach();
    } else {
        api_.configureDevice(base_, puddleId_, key_, seed_);
        set(QStringLiteral("waiting"));
        poll_.start();
        poll();
    }
}

bool Phone::save()
{
    QDir().mkpath(dir_);
    QSaveFile f(dir_ + QStringLiteral("/device.json"));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); /* the key is this phone's password */
    const QJsonObject o{ { "base", base_.toString() },
                         { "puddle", puddleId_ },
                         { "puddle_key", QString::fromLatin1(key_.toHex()) },
                         { "seed", QString::fromLatin1(seed_.toHex()) },
                         { "state", state_ == QLatin1String("ready") ? "active" : "pending" } };
    f.write(QJsonDocument(o).toJson());
    return f.commit();
}

void Phone::pair(const QString &link, const QString &name)
{
    ApiClient::PairingLink l;
    if (!ApiClient::parsePairingLink(link, &l))
        return set(QStringLiteral("unpaired"), tr("That isn't a pairing link. On your computer, open Puddle, Settings, \"Add a phone\", and copy the link it shows."));
    if (name.trimmed().isEmpty() || name.trimmed().toUtf8().size() > 64)
        return set(QStringLiteral("unpaired"), tr("Give this phone a name (up to 64 characters)."));
    const QByteArray seed = ApiClient::newDeviceSeed();
    if (seed.size() != 32)
        return set(QStringLiteral("unpaired"), tr("Could not make a key for this phone."));
    set(QStringLiteral("pairing"));
    api_.pair(l, name, seed, [this, l, seed](const ApiClient::Paired &p) {
        if (!p.ok)
            return set(QStringLiteral("unpaired"), p.error);
        base_ = l.base;
        puddleId_ = l.puddleId;
        key_ = p.puddleKey;
        seed_ = seed;
        check_ = p.check;
        set(QStringLiteral("waiting"));
        if (!save())
            error_ = tr("Paired, but this phone could not save it: it will have to pair again if the app is closed.");
        api_.configureDevice(base_, puddleId_, key_, seed_);
        poll_.start();
        emit changed();
    });
}

void Phone::poll()
{
    if (state_ != QLatin1String("waiting"))
        return poll_.stop();
    api_.get(QStringLiteral("/device/v1/status"), [this](int status, const QJsonDocument &doc) {
        if (state_ != QLatin1String("waiting"))
            return;
        if (status == 200 && doc.object().value("state").toString() == QLatin1String("active")) {
            poll_.stop();
            attach();
            save();
            return;
        }
        const QString why = status == 200 ? QString() : api_.lastProblem();
        if (why != error_) {
            error_ = why;
            emit changed();
        }
    });
}

void Phone::attach()
{
    if (!chats_->attachDevice(base_, puddleId_, key_, seed_))
        return set(QStringLiteral("unpaired"), tr("What this phone knew about its Puddle was damaged. Pair it again."));
    set(QStringLiteral("ready"));
}

void Phone::forget()
{
    poll_.stop();
    QFile::remove(dir_ + QStringLiteral("/device.json"));
    base_ = QUrl();
    puddleId_.clear();
    key_.clear();
    seed_.clear();
    check_.clear();
    set(QStringLiteral("unpaired"));
}
