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

    // Qt's network manager keeps about 8 KB for every TLS connection it opens until it is destroyed (seen with Qt 6.12),
    // and a Puddle closes the connection after each answer: polling made the window grow by gigabytes over a few days.
    // So each manager is used for this many requests and then replaced; an old one goes once its last reply has.
    static constexpr int UsesPerManager = 256;

    // `certPem`: the Puddle's own certificate file. `token`: the owner token.
    bool configure(const QUrl &base, const QByteArray &token, const QString &certPemPath);
    bool ready() const { return ready_; }

    void get(const QString &path, Reply done);
    void post(const QString &path, const QJsonObject &body, Reply done);
    // POST `bytes` as they are (a chunk of a file being uploaded).
    void postBytes(const QString &path, const QByteArray &bytes, Reply done);

    // GET `path` into the file `dest`, written as it arrives (a file of any size): `progress(received, total)` on the way,
    // `done(ok)` at the end. Nothing is left at `dest` unless the whole of it came.
    void download(const QString &path, const QString &dest, std::function<void(qint64, qint64)> progress, std::function<void(bool)> done);

    // A picture or other file the Puddle holds, by its path ("/api/v1/media?hash=..."), as raw bytes.
    void getBytes(const QString &path, std::function<void(int status, const QByteArray &bytes)> done);

private:
    QNetworkRequest request(const QString &path) const;
    void watch(QNetworkReply *r, Reply done);

    QNetworkAccessManager *manager();
    void renew();              // a new manager now: also how kept connections (and the trust they started with) are dropped
    QNetworkAccessManager *net_ = nullptr;
    int uses_ = 0;
    QUrl base_;
    QByteArray token_;
    QSslConfiguration ssl_;
    bool ready_ = false;
};
