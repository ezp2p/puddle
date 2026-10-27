#pragma once

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QObject>
#include <QUrl>
#include <functional>

// Talks to the owner API of a Puddle over HTTPS, the way a phone client does. The Puddle's own certificate is
// the only one trusted (pinned), and the owner token goes with every request. Only GET and POST are used.
class ApiClient : public QObject {
    Q_OBJECT
public:
    // status 0 = the Puddle could not be reached; `json` is null when the body was not JSON (e.g. 204).
    using Reply = std::function<void(int status, const QJsonDocument &json)>;

    explicit ApiClient(QObject *parent = nullptr);

    // `certPem`: the Puddle's own certificate file. `token`: the owner token.
    bool configure(const QUrl &base, const QByteArray &token, const QString &certPemPath);
    bool ready() const { return ready_; }

    void get(const QString &path, Reply done);
    void post(const QString &path, const QJsonObject &body, Reply done);

    // A picture or other file the Puddle holds, by its path ("/api/v1/media?hash=..."), as raw bytes.
    void getBytes(const QString &path, std::function<void(int status, const QByteArray &bytes)> done);

private:
    QNetworkRequest request(const QString &path) const;
    void watch(QNetworkReply *r, Reply done);

    QNetworkAccessManager net_;
    QUrl base_;
    QByteArray token_;
    QSslConfiguration ssl_;
    bool ready_ = false;
};
