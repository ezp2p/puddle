#pragma once

// The same User as objects/user.hpp, seen from an app: filled from the owner API's JSON, and its actions are
// HTTPS calls. One instance per id (Client keeps them), so everything showing Bob updates when Bob changes.
//
//     User *bob = client->user(bobId);
//     bob->message("hi");
//     label->setText(bob->displayName());       // or in QML: text: bob.displayName

#include <QObject>
#include <QString>

namespace puddle::client {

class Client;
class Pending;

class User : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString id READ id CONSTANT)
    Q_PROPERTY(QString displayName READ displayName NOTIFY changed)
    Q_PROPERTY(QString avatar READ avatar NOTIFY changed)
    Q_PROPERTY(QString streamState READ streamState NOTIFY changed)
    Q_PROPERTY(bool blocked READ blocked NOTIFY changed)
public:
    QString id() const { return id_; }
    QString displayName() const;
    QString avatar() const;
    QString streamState() const;
    bool blocked() const;

    Q_INVOKABLE Pending *message(const QString &body, const QString &replyTo = {}); // POST conversations/{id}/messages/send
    Q_INVOKABLE Pending *block();                                                   // POST users/{id}/block
    Q_INVOKABLE Pending *unblock();

signals:
    void changed();

private:
    friend class Client;
    User(Client *client, QString id);
    void update(const QJsonObject &json); // from a reply or an event; emits changed()

    Client *client_;
    QString id_;
};

} // namespace puddle::client
