#pragma once

#include <QObject>
#include <QTimer>

#include <functional>

#include "apiclient.h"
#include "callleg.h"

// The call going on, for the window (docs/calls.md, "The owner's side"): read from the owner API (/api/v1/calls), which changes
// a number (`version`) whenever any call does, so the timer asks for little until something happens. The sound is not here:
// the Backend plays it (callaudio.h), since only it holds the Puddle the frames go through, or, on a phone, the leg.
//
// A call is had by one of the owner's devices: the window, or the phone that started or answered it. Every device rings;
// the others show where it went and can hang it up. On a phone (setDevice), the call it has gets a leg of its own to the
// Puddle (callleg.h), set up here.
class Calls : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)         // "" for no call; calling, ringing, connecting, active
    Q_PROPERTY(QString callId READ callId NOTIFY changed)
    Q_PROPERTY(QString user READ user NOTIFY changed)           // the friend
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool incoming READ incoming NOTIFY changed)
    Q_PROPERTY(qint64 answeredAt READ answeredAt NOTIFY changed) // ms since the epoch, 0 until it connected
    Q_PROPERTY(bool here READ here NOTIFY changed)              // this device has the call (ringing is everywhere's)
    Q_PROPERTY(QString elsewhere READ elsewhere NOTIFY changed)  // where it is if not here, in words: "on Pixel", "on the computer"
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)     // the last thing that went wrong starting or answering, in words
public:
    explicit Calls(ApiClient *api, QObject *parent = nullptr);

    QString state() const { return state_; }
    QString callId() const { return id_; }
    QString user() const { return user_; }
    QString name() const { return nameOf_ ? nameOf_(user_) : user_; }
    bool incoming() const { return incoming_; }
    qint64 answeredAt() const { return answered_; }
    bool here() const { return here_; }
    QString elsewhere() const;
    bool muted() const { return muted_; }
    QString error() const { return error_; }
    quint64 version() const { return version_; }

    void setNameOf(std::function<QString(const QString &)> fn) { nameOf_ = std::move(fn); }
    // A phone: this is its device id, and `host` its Puddle's address as it reaches it. Its calls get a leg.
    void setDevice(const QString &id, const QString &host);
    CallLeg *leg() { return leg_; } // nullptr on the desktop
    void start() { timer_.start(); refresh(); }
    // No more asking (the app is going): a phone's leg closes, and its Puddle ends a call it had once it hears nothing.
    void stop() { timer_.stop(); if (leg_) leg_->close(); }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void call(const QString &user);
    Q_INVOKABLE void accept();
    Q_INVOKABLE void decline();
    Q_INVOKABLE void hangUp();
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE void toggleMute() { setMuted(!muted_); }
    // Words for how a call ended, as a chat shows it
    Q_INVOKABLE QString describe(const QString &reason, bool incoming, const QString &name, qint64 seconds) const;

signals:
    void changed();
    void mutedChanged();
    void errorChanged();
    void ringing(const QString &user);  // a friend's call just started ringing here
    void historyChanged();               // a call ended (or began): the notes in a chat may have changed

private:
    void act(const QString &verb);
    void take(const QJsonObject &call);
    void setError(const QString &e);
    void followLeg();
    ApiClient *api_;
    CallLeg *leg_ = nullptr;
    QString myDevice_, host_, device_, deviceName_;
    bool here_ = false;
    QTimer timer_;
    std::function<QString(const QString &)> nameOf_;
    quint64 version_ = 0;
    bool busy_ = false;
    QString state_, id_, user_, error_;
    bool incoming_ = false, muted_ = false;
    qint64 answered_ = 0;
};
