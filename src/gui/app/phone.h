#pragma once

#include <QObject>
#include <QTimer>
#include <QUrl>

#include "apiclient.h"

class Chats;

// The app on a phone: a thin client of the owner's own Puddle (docs/identity.md, "Devices"). It holds no keys of the
// identity and no chats, only a key of its own and what it learned at pairing, in <dir>/device.json (readable by the app
// alone). States:
//   unpaired  nothing yet: the owner pastes (or scans) the pairing link from the desktop app
//   pairing   asking the Puddle
//   waiting   paired, until the owner accepts this phone on the desktop (comparing `check`)
//   ready     the Chats talk to the Puddle as this phone
class Phone : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString check READ check NOTIFY changed)       // six digits the desktop app shows too
    Q_PROPERTY(QString error READ error NOTIFY changed)       // what went wrong, in words; "" if nothing
    Q_PROPERTY(QString address READ address NOTIFY changed)   // where its Puddle is
    Q_PROPERTY(QString puddleId READ puddleId NOTIFY changed)
public:
    Phone(Chats *chats, const QString &dir, QObject *parent = nullptr);

    QString state() const { return state_; }
    QString check() const { return check_; }
    QString error() const { return error_; }
    QString address() const { return base_.authority(); }
    QString puddleId() const { return puddleId_; }
    void setPollInterval(int ms) { poll_.setInterval(ms); }

    Q_INVOKABLE void pair(const QString &link, const QString &name);
    Q_INVOKABLE void forget();                 // start over (this phone's key is thrown away)
    Q_INVOKABLE QString defaultName() const;   // what to call this phone, to begin with

signals:
    void changed();

private:
    void load();
    bool save();
    void poll();
    void attach();
    void set(const QString &state, const QString &error = QString());

    Chats *chats_;
    QString dir_;
    ApiClient api_;  // pairing, and asking how it stands while waiting
    QTimer poll_;
    QString state_ = QStringLiteral("unpaired"), check_, error_, puddleId_;
    QUrl base_;
    QByteArray key_, seed_;
};
