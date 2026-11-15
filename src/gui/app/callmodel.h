#pragma once

#include <QHash>
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
//
// A group call (docs/calls.md, "Group calls") is the same here, with `group` set: started from the group's chat, joined from
// its ringing or from the chat while it goes on (groupCall), and left by hanging up while the others carry on.
class Calls : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)         // "" for no call; calling, ringing, connecting, active
    Q_PROPERTY(QString callId READ callId NOTIFY changed)
    Q_PROPERTY(QString user READ user NOTIFY changed)           // the friend; who started it, in a group call
    Q_PROPERTY(QString group READ group NOTIFY changed)         // the group, in a group call; "" with a friend
    Q_PROPERTY(int others READ others NOTIFY changed)           // in a group call: how many others are in it
    Q_PROPERTY(QString name READ name NOTIFY changed)           // the friend's name, or the group's
    Q_PROPERTY(bool incoming READ incoming NOTIFY changed)
    Q_PROPERTY(qint64 answeredAt READ answeredAt NOTIFY changed) // ms since the epoch, 0 until it connected
    Q_PROPERTY(bool here READ here NOTIFY changed)              // this device has the call (ringing is everywhere's)
    Q_PROPERTY(QString elsewhere READ elsewhere NOTIFY changed)  // where it is if not here, in words: "on Pixel", "on the computer"
    Q_PROPERTY(bool muted READ muted NOTIFY mutedChanged)
    Q_PROPERTY(bool camera READ camera NOTIFY cameraChanged)    // this end's camera is on (off at the start of every call)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)     // the last thing that went wrong starting or answering, in words
public:
    explicit Calls(ApiClient *api, QObject *parent = nullptr);

    QString state() const { return state_; }
    QString callId() const { return id_; }
    QString user() const { return user_; }
    QString group() const { return group_; }
    int others() const { return others_; }
    QString name() const { return nameOf_ ? nameOf_(group_.isEmpty() ? user_ : group_) : user_; }
    bool incoming() const { return incoming_; }
    qint64 answeredAt() const { return answered_; }
    bool here() const { return here_; }
    QString elsewhere() const;
    bool muted() const { return muted_; }
    bool camera() const { return camera_; }
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
    // Start a call in a group, or join the one going on there
    Q_INVOKABLE void callGroup(const QString &group);
    // How many are in the call going on in `group` (0: none); in it or not
    Q_INVOKABLE int groupCall(const QString &group) const { return groupCalls_.value(group, 0); }
    Q_INVOKABLE void accept();
    Q_INVOKABLE void decline();
    Q_INVOKABLE void hangUp();
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE void toggleMute() { setMuted(!muted_); }
    Q_INVOKABLE void setCamera(bool on);
    Q_INVOKABLE void toggleCamera() { setCamera(!camera_); }
    // Who has number `slot` in a group call (a phone's video from "slot:<n>"), "" if nobody
    Q_INVOKABLE QString slotUser(int slot) const { return slots_.value(slot); }
    // Words for how a call ended, as a chat shows it
    Q_INVOKABLE QString describe(const QString &reason, bool incoming, const QString &name, qint64 seconds) const;
    // ... and a group's, as its chat shows it: `name` is who started it
    Q_INVOKABLE QString describeGroup(const QString &reason, bool incoming, const QString &name, qint64 seconds) const;

signals:
    void changed();
    void mutedChanged();
    void cameraChanged();
    void errorChanged();
    void ringing(const QString &user, const QString &group); // a friend's call (or a call in a group) just started ringing here
    void groupCallsChanged();            // the calls going on in groups
    void historyChanged();               // a call ended (or began): the notes in a chat may have changed

private:
    void act(const QString &verb);
    void start(const QJsonObject &body);
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
    QString state_, id_, user_, group_, error_;
    int others_ = 0;
    QHash<QString, int> groupCalls_;
    bool incoming_ = false, muted_ = false, camera_ = false;
    QHash<int, QString> slots_;
    qint64 answered_ = 0;
};
