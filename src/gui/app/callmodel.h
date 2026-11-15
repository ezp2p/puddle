#pragma once

#include <QObject>
#include <QTimer>

#include <functional>

#include "apiclient.h"

// The call going on, for the window (docs/calls.md, "The owner's side"): read from the owner API (/api/v1/calls), which changes
// a number (`version`) whenever any call does, so the timer asks for little until something happens. The sound is not here:
// the Backend plays it (callaudio.h), since only it holds the Puddle the frames go through.
class Calls : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)         // "" for no call; calling, ringing, connecting, active
    Q_PROPERTY(QString callId READ callId NOTIFY changed)
    Q_PROPERTY(QString user READ user NOTIFY changed)           // the friend
    Q_PROPERTY(QString name READ name NOTIFY changed)
    Q_PROPERTY(bool incoming READ incoming NOTIFY changed)
    Q_PROPERTY(qint64 answeredAt READ answeredAt NOTIFY changed) // ms since the epoch, 0 until it connected
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
    bool muted() const { return muted_; }
    QString error() const { return error_; }
    quint64 version() const { return version_; }

    void setNameOf(std::function<QString(const QString &)> fn) { nameOf_ = std::move(fn); }
    void start() { timer_.start(); refresh(); }

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
    ApiClient *api_;
    QTimer timer_;
    std::function<QString(const QString &)> nameOf_;
    quint64 version_ = 0;
    bool busy_ = false;
    QString state_, id_, user_, error_;
    bool incoming_ = false, muted_ = false;
    qint64 answered_ = 0;
};
