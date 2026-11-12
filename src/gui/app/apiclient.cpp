#include "apiclient.h"

#include <QFile>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslError>

ApiClient::ApiClient(QObject *parent) : QObject(parent) {}

bool ApiClient::configure(const QUrl &base, const QByteArray &token, const QString &certPemPath)
{
    ready_ = false;
    QFile f(certPemPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QList<QSslCertificate> certs = QSslCertificate::fromData(f.readAll(), QSsl::Pem);
    if (certs.isEmpty() || token.isEmpty() || !base.isValid())
        return false;
    base_ = base;
    token_ = token;
    ssl_ = QSslConfiguration::defaultConfiguration();
    ssl_.setCaCertificates(certs); // only this one: nobody else's certificate will do
    ssl_.setPeerVerifyMode(QSslSocket::VerifyPeer);
    ready_ = true;
    return true;
}

QNetworkRequest ApiClient::request(const QString &path) const
{
    QNetworkRequest req(QUrl(base_.toString() + path));
    req.setSslConfiguration(ssl_);
    req.setRawHeader("Authorization", "Bearer " + token_);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(15000);
    return req;
}

// The certificate is checked against the pinned one; we reach the Puddle by an address that is not in it.
static void allowHostMismatch(QNetworkReply *r)
{
    QObject::connect(r, &QNetworkReply::sslErrors, r, [r](const QList<QSslError> &errors) {
        QList<QSslError> ignore;
        for (const QSslError &e : errors)
            if (e.error() == QSslError::HostNameMismatch)
                ignore << e;
        if (ignore.size() == errors.size())
            r->ignoreSslErrors(ignore);
    });
}

void ApiClient::watch(QNetworkReply *r, Reply done)
{
    allowHostMismatch(r);
    connect(r, &QNetworkReply::finished, this, [r, done = std::move(done)] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        r->deleteLater();
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError)
            doc = QJsonDocument();
        done(status, doc);
    });
}

void ApiClient::get(const QString &path, Reply done)
{
    if (!ready_)
        return done(0, QJsonDocument());
    watch(net_.get(request(path)), std::move(done));
}

void ApiClient::post(const QString &path, const QJsonObject &body, Reply done)
{
    if (!ready_)
        return done(0, QJsonDocument());
    QNetworkRequest req = request(path);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    watch(net_.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact)), std::move(done));
}

void ApiClient::postBytes(const QString &path, const QByteArray &bytes, Reply done)
{
    if (!ready_)
        return done(0, QJsonDocument());
    QNetworkRequest req = request(path);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    req.setTransferTimeout(60000); // a chunk is 1 MiB: room for a slow link to a Puddle elsewhere
    watch(net_.post(req, bytes), std::move(done));
}

void ApiClient::download(const QString &path, const QString &dest, std::function<void(qint64, qint64)> progress, std::function<void(bool)> done)
{
    if (!ready_)
        return done(false);
    auto *file = new QFile(dest + ".part");
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        return done(false);
    }
    QNetworkRequest req = request(path);
    req.setTransferTimeout(60000); // without a byte for a minute: given up
    QNetworkReply *r = net_.get(req);
    allowHostMismatch(r);
    connect(r, &QNetworkReply::readyRead, this, [r, file] { file->write(r->readAll()); });
    connect(r, &QNetworkReply::downloadProgress, this, [progress](qint64 got, qint64 total) { if (progress) progress(got, total); });
    connect(r, &QNetworkReply::finished, this, [r, file, dest, done = std::move(done)] {
        file->write(r->readAll());
        const bool ok = r->error() == QNetworkReply::NoError && r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && file->flush();
        file->close();
        r->deleteLater();
        if (ok) {
            QFile::remove(dest);
            file->rename(dest);
        } else {
            file->remove();
        }
        const bool there = ok && QFile::exists(dest);
        delete file;
        done(there);
    });
}

void ApiClient::getBytes(const QString &path, std::function<void(int, const QByteArray &)> done)
{
    if (!ready_)
        return done(0, QByteArray());
    QNetworkReply *r = net_.get(request(path));
    allowHostMismatch(r);
    connect(r, &QNetworkReply::finished, this, [r, done = std::move(done)] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        r->deleteLater();
        done(status, body);
    });
}
