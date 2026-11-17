#include "apiclient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslError>
#include <QUrlQuery>
#include <memory>

#include "core/devices.h"
#include "core/identity.h"
#include "http/reqsig.h"

static constexpr qint64 ProofMs = 10 * 60 * 1000; // as the Puddles' outboxes: docs/streams.md, "Proving who answers"

ApiClient::ApiClient(QObject *parent) : QObject(parent)
{
    renew(net_);
    renew(open_);
}

void ApiClient::renew(QNetworkAccessManager *&m)
{
    if (m) {
        if (m->property("inflight").toInt() == 0)
            m->deleteLater();
        else
            m->setProperty("retired", true); /* deleted when its last reply finishes; deleting it now would drop answers */
    }
    QNetworkAccessManager *fresh = new QNetworkAccessManager(this);
    connect(fresh, &QNetworkAccessManager::finished, fresh, [fresh] {
        const int left = fresh->property("inflight").toInt() - 1;
        fresh->setProperty("inflight", left);
        if (left == 0 && fresh->property("retired").toBool())
            fresh->deleteLater();
    });
    m = fresh;
    (&m == &net_ ? netUses_ : openUses_) = 0;
}

QNetworkAccessManager *ApiClient::manager(bool open)
{
    QNetworkAccessManager *&m = open ? open_ : net_;
    int &uses = open ? openUses_ : netUses_;
    if (uses >= UsesPerManager)
        renew(m);
    uses++;
    m->setProperty("inflight", m->property("inflight").toInt() + 1);
    return m;
}

bool ApiClient::configure(const QUrl &base, const QByteArray &token, const QString &certPemPath)
{
    ready_ = false;
    device_ = false;
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
    renew(net_); /* a kept connection would go on with what it trusted before */
    ready_ = true;
    return true;
}

bool ApiClient::configureDevice(const QUrl &base, const QString &puddleId, const QByteArray &puddleKey, const QByteArray &deviceSeed)
{
    ready_ = false;
    puddle_devkey k;
    if (!base.isValid() || puddleId.size() != PUDDLE_USER_ID_LEN || puddleKey.size() != 32 || deviceSeed.size() != 32 ||
        puddle_devkey_from_seed(&k, reinterpret_cast<const unsigned char *>(deviceSeed.constData())) != 0)
        return false;
    base_ = base;
    token_.clear();
    device_ = true;
    puddleId_ = puddleId;
    puddleKey_ = puddleKey;
    seed_ = deviceSeed;
    proven_ = QSslCertificate();
    provenUntil_ = 0;
    renew(net_);
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

static QJsonDocument jsonOf(const QByteArray &body)
{
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    return pe.error == QJsonParseError::NoError ? doc : QJsonDocument();
}

void ApiClient::watch(QNetworkReply *r, Reply done)
{
    allowHostMismatch(r);
    connect(r, &QNetworkReply::finished, this, [r, done = std::move(done)] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        r->deleteLater();
        done(status, jsonOf(body));
    });
}

void ApiClient::issue(const QByteArray &method, const QString &path, const QByteArray &body, const QByteArray &contentType, int timeoutMs,
                      RawReply done)
{
    if (!ready_)
        return done(0, QByteArray());
    if (device_) {
        Pending p;
        p.method = method;
        p.body = body;
        p.contentType = contentType;
        p.path = path;
        p.done = std::move(done);
        p.timeoutMs = timeoutMs;
        return deviceSend(std::move(p));
    }
    QNetworkRequest req = request(path);
    req.setTransferTimeout(timeoutMs);
    QNetworkReply *r;
    if (method == "POST") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        r = manager(false)->post(req, body);
    } else {
        r = manager(false)->get(req);
    }
    allowHostMismatch(r);
    connect(r, &QNetworkReply::finished, this, [r, done = std::move(done)] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray b = r->readAll();
        r->deleteLater();
        done(status, b);
    });
}

void ApiClient::get(const QString &path, Reply done)
{
    issue("GET", path, QByteArray(), QByteArray(), 15000, [done = std::move(done)](int status, const QByteArray &b) { done(status, jsonOf(b)); });
}

void ApiClient::post(const QString &path, const QJsonObject &body, Reply done)
{
    issue("POST", path, QJsonDocument(body).toJson(QJsonDocument::Compact), "application/json", 15000,
          [done = std::move(done)](int status, const QByteArray &b) { done(status, jsonOf(b)); });
}

void ApiClient::postBytes(const QString &path, const QByteArray &bytes, Reply done)
{
    // a chunk is 1 MiB: room for a slow link to a Puddle elsewhere
    issue("POST", path, bytes, "application/octet-stream", 60000,
          [done = std::move(done)](int status, const QByteArray &b) { done(status, jsonOf(b)); });
}

void ApiClient::getBytes(const QString &path, std::function<void(int, const QByteArray &)> done)
{
    issue("GET", path, QByteArray(), QByteArray(), 15000, std::move(done));
}

void ApiClient::download(const QString &path, const QString &dest, std::function<void(qint64, qint64)> progress, std::function<void(bool)> done)
{
    if (!ready_)
        return done(false);
    if (device_) {
        Pending p;
        p.method = "GET";
        p.path = path;
        p.timeoutMs = 60000;
        p.dest = dest;
        p.progress = std::move(progress);
        p.fetched = std::move(done);
        return deviceSend(std::move(p));
    }
    auto *file = new QFile(dest + ".part");
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        return done(false);
    }
    QNetworkRequest req = request(path);
    req.setTransferTimeout(60000); // without a byte for a minute: given up
    QNetworkReply *r = manager(false)->get(req);
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

// ---- a phone ----

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

static QByteArray fingerprintOf(QNetworkReply *r)
{
    QSslCertificate c = r->sslConfiguration().peerCertificate();
    /* Qt puts the certificate only on the reply that opened the connection; later ones on a kept connection carry none.
     * A pinned request's connection trusts nothing but the certificate it was pinned to, and every kept connection is
     * dropped whenever that changes (forgetProof), so a pinned reply that shows none came over that one. */
    if (c.isNull())
        c = r->property("puddlePinned").value<QSslCertificate>();
    return c.isNull() ? QByteArray() : c.digest(QCryptographicHash::Sha256).toHex();
}

void ApiClient::setProblem(const QString &why)
{
    problem_ = why;
    if (!why.isEmpty())
        emit problem(why);
}

QNetworkReply *ApiClient::signedRequest(const QByteArray &method, const QString &path, const QByteArray &body, const QByteArray &contentType,
                                        int timeoutMs, bool pinned, QByteArray *nonce)
{
    const QUrl url(base_.toString() + path);
    const QByteArray target = url.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority); // path and query, exactly as sent
    puddle_devkey k;
    char sig[1024];
    puddle_reqsig parsed;
    if (puddle_devkey_from_seed(&k, reinterpret_cast<const unsigned char *>(seed_.constData())) != 0 ||
        puddle_reqsig_sign_as(k.id, k.pub, puddle_devkey_sign, &k, method.constData(), target.constData(), body.constData(), size_t(body.size()),
                              puddleId_.toLatin1().constData(), 0, sig, sizeof sig) != 0 ||
        puddle_reqsig_parse(sig, &parsed) != 0)
        return nullptr;
    *nonce = QByteArray(parsed.nonce);
    QNetworkRequest req(url);
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    if (pinned) {
        ssl.setCaCertificates({ proven_ }); // the certificate it signed for, and no other: nothing goes anywhere else
        ssl.setPeerVerifyMode(QSslSocket::VerifyPeer);
    } else {
        ssl.setPeerVerifyMode(QSslSocket::VerifyNone); // the handshake carries nothing; its receipt says whose this is
        req.setRawHeader("Connection", "close");       // a connection of its own, so its reply shows the certificate
    }
    req.setSslConfiguration(ssl);
    req.setRawHeader("X-Puddle-Signature", sig);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(timeoutMs);
    /* unpinned (the handshake, pairing) on a manager of its own: Qt keeps connections, with the trust they started with */
    QNetworkAccessManager &via = *manager(!pinned);
    QNetworkReply *r;
    if (method == "POST") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType.isEmpty() ? QByteArray("application/json") : contentType);
        r = via.post(req, body);
    } else {
        r = via.get(req);
    }
    if (pinned) {
        allowHostMismatch(r);
        r->setProperty("puddlePinned", QVariant::fromValue(proven_));
    }
    return r;
}

int ApiClient::checkReceipt(QNetworkReply *r, int status, const QByteArray &body, const QByteArray &nonce, const QByteArray &key,
                            const QByteArray &bodyHash)
{
    const QByteArray receipt = r->rawHeader("X-Puddle-Receipt");
    const QByteArray fp = fingerprintOf(r);
    const QByteArray hash = bodyHash.isEmpty() ? QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() : bodyHash;
    const int rc = puddle_receipt_verify_hash(receipt.isEmpty() ? nullptr : receipt.constData(), puddleId_.toLatin1().constData(),
                                              reinterpret_cast<const unsigned char *>(key.constData()), nonce.constData(), status,
                                              hash.constData(), fp.isEmpty() ? nullptr : fp.constData());
    const QString where = base_.authority();
    switch (rc) {
    case PUDDLE_RECEIPT_OK: problem_.clear(); break;
    case PUDDLE_RECEIPT_MISSING:
        problem_ = status == 404 ? tr("Your Puddle at %1 didn't recognise this phone: it isn't your Puddle, or this phone was removed from it.").arg(where)
                                 : tr("Something at %1 answered, but it isn't your Puddle.").arg(where);
        break;
    case PUDDLE_RECEIPT_WRONG_KEY: problem_ = tr("What answered at %1 couldn't prove it is your Puddle.").arg(where); break;
    case PUDDLE_RECEIPT_OTHER_CERT: problem_ = tr("The connection to %1 was intercepted. Nothing was sent.").arg(where); break;
    default: problem_ = tr("What answered at %1 couldn't prove it is your Puddle (its signature doesn't check out).").arg(where); break;
    }
    return rc;
}

void ApiClient::deviceSend(Pending p)
{
    if (proven_.isNull() || provenUntil_ <= nowMs()) {
        waiting_ << std::move(p);
        handshake();
        return;
    }
    if (!p.dest.isEmpty())
        return deviceDownload(std::move(p));
    QByteArray nonce;
    QNetworkReply *r = signedRequest(p.method, p.path, p.body, p.contentType, p.timeoutMs, true, &nonce);
    if (!r)
        return p.done(0, QByteArray());
    connect(r, &QNetworkReply::finished, this, [this, r, nonce, p = std::move(p)]() mutable {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        const QNetworkReply::NetworkError err = r->error();
        r->deleteLater();
        if ((err == QNetworkReply::SslHandshakeFailedError || status == 0) && p.tries > 0) {
            /* most likely another certificate since (it may show as any failure to connect): prove it before anything goes
             * to it. If the Puddle is simply not there, the handshake says so. */
            forgetProof();
            p.tries--;
            return deviceSend(std::move(p));
        }
        if (status == 0) {
            problem_ = tr("Your Puddle can't be reached at %1.").arg(base_.authority());
            return p.done(0, QByteArray());
        }
        const int rc = checkReceipt(r, status, body, nonce, puddleKey_);
        if (rc == PUDDLE_RECEIPT_OK) {
            provenUntil_ = nowMs() + ProofMs;
            return p.done(status, body);
        }
        forgetProof();
        if (rc == PUDDLE_RECEIPT_WRONG_KEY && p.tries > 0) { /* it may have rolled its keys: the handshake learns the new one */
            p.tries--;
            return deviceSend(std::move(p));
        }
        setProblem(problem_);
        p.done(0, QByteArray());
    });
}

void ApiClient::forgetProof()
{
    proven_ = QSslCertificate();
    renew(net_); /* kept connections were pinned to it */
}

/* A phone's download: written to "<dest>.part" as it comes and hashed on the way, then kept only if the Puddle signed for
 * exactly those bytes. Over a connection showing the proven certificate, like everything else. */
void ApiClient::deviceDownload(Pending p)
{
    auto *file = new QFile(p.dest + ".part");
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        return p.fetched(false);
    }
    QByteArray nonce;
    QNetworkReply *r = signedRequest("GET", p.path, QByteArray(), QByteArray(), p.timeoutMs, true, &nonce);
    if (!r) {
        file->remove();
        delete file;
        return p.fetched(false);
    }
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    connect(r, &QNetworkReply::readyRead, this, [r, file, hash] {
        const QByteArray got = r->readAll();
        hash->addData(got);
        file->write(got);
    });
    auto progress = p.progress;
    connect(r, &QNetworkReply::downloadProgress, this, [progress](qint64 got, qint64 total) { if (progress) progress(got, total); });
    connect(r, &QNetworkReply::finished, this, [this, r, file, hash, nonce, p = std::move(p)]() mutable {
        const QByteArray rest = r->readAll();
        hash->addData(rest);
        file->write(rest);
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool came = r->error() == QNetworkReply::NoError && status == 200 && file->flush();
        file->close();
        r->deleteLater();
        bool ok = false;
        if (came) {
            const int rc = checkReceipt(r, status, QByteArray(), nonce, puddleKey_, hash->result().toHex());
            if (rc == PUDDLE_RECEIPT_OK) {
                provenUntil_ = nowMs() + ProofMs;
                ok = true;
            } else {
                forgetProof();
                setProblem(problem_);
            }
        }
        if (ok) {
            QFile::remove(p.dest);
            file->rename(p.dest);
        } else {
            file->remove();
        }
        const bool there = ok && QFile::exists(p.dest);
        delete file;
        p.fetched(there);
    });
}

void ApiClient::handshake()
{
    if (shaking_)
        return;
    shaking_ = true;
    QByteArray nonce;
    QNetworkReply *r = signedRequest("GET", QStringLiteral("/device/v1/status"), QByteArray(), QByteArray(), 15000, false, &nonce);
    if (!r)
        return handshakeDone(false);
    connect(r, &QNetworkReply::finished, this, [this, r, nonce] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        r->deleteLater();
        if (status == 0) {
            problem_ = tr("Your Puddle can't be reached at %1.").arg(base_.authority());
            return handshakeDone(false);
        }
        int rc = checkReceipt(r, status, body, nonce, puddleKey_);
        if (rc == PUDDLE_RECEIPT_WRONG_KEY) {
            /* signed by a key we don't hold: if the chain it brings is our Puddle's (by its id) and its key signed this, it rolled */
            const QByteArray chain = jsonOf(body).object().value(QStringLiteral("chain")).toString().toUtf8();
            puddle_chain_head h;
            char err[200];
            if (!chain.isEmpty() &&
                puddle_chain_verify(chain.constData(), size_t(chain.size()), puddleId_.toLatin1().constData(), &h, err, sizeof err) == 0) {
                const QByteArray key(reinterpret_cast<const char *>(h.sign_pub), 32);
                if (checkReceipt(r, status, body, nonce, key) == PUDDLE_RECEIPT_OK) {
                    puddleKey_ = key;
                    rc = PUDDLE_RECEIPT_OK;
                    emit puddleKeyChanged(key);
                }
            }
        }
        if (rc == PUDDLE_RECEIPT_OK && status == 200) {
            const QSslCertificate c = r->sslConfiguration().peerCertificate();
            if (c != proven_)
                renew(net_); /* kept connections carry the certificate they were pinned to */
            proven_ = c;
            provenUntil_ = nowMs() + ProofMs;
            return handshakeDone(true);
        }
        if (rc == PUDDLE_RECEIPT_OK) /* it is our Puddle, and it says no (this phone was removed) */
            problem_ = tr("Your Puddle no longer lets this phone in.");
        handshakeDone(false);
    });
}

void ApiClient::handshakeDone(bool ok)
{
    shaking_ = false;
    QList<Pending> all;
    all.swap(waiting_);
    if (!ok)
        setProblem(problem_);
    for (Pending &p : all) {
        if (ok)
            deviceSend(std::move(p));
        else if (p.fetched)
            p.fetched(false);
        else
            p.done(0, QByteArray());
    }
}

// ---- pairing ----

bool ApiClient::parsePairingLink(const QString &link, PairingLink *out)
{
    const QUrl u(link.trimmed());
    if (!u.isValid() || u.scheme() != QLatin1String("puddle-pair") || u.host().isEmpty())
        return false;
    const QUrlQuery q(u);
    const QString id = q.queryItemValue(QStringLiteral("id")), secret = q.queryItemValue(QStringLiteral("secret"));
    static const QString b32 = QStringLiteral("abcdefghijklmnopqrstuvwxyz234567"), hex = QStringLiteral("0123456789abcdef");
    if (id.size() != PUDDLE_USER_ID_LEN || secret.size() != PUDDLE_PAIR_SECRET_LEN)
        return false;
    for (QChar c : id)
        if (!b32.contains(c))
            return false;
    for (QChar c : secret)
        if (!hex.contains(c))
            return false;
    QUrl base;
    base.setScheme(QStringLiteral("https"));
    base.setHost(u.host());
    base.setPort(u.port(443));
    out->base = base;
    out->puddleId = id;
    out->secret = secret;
    return true;
}

QByteArray ApiClient::newDeviceSeed()
{
    puddle_devkey k;
    if (puddle_devkey_generate(&k) != 0)
        return QByteArray();
    return QByteArray(reinterpret_cast<const char *>(k.priv), 32);
}

QString ApiClient::deviceIdOf(const QByteArray &seed)
{
    puddle_devkey k;
    if (seed.size() != 32 || puddle_devkey_from_seed(&k, reinterpret_cast<const unsigned char *>(seed.constData())) != 0)
        return QString();
    return QString::fromLatin1(k.id);
}

QString ApiClient::checkOf(const QByteArray &seed)
{
    puddle_devkey k;
    char c[7];
    if (seed.size() != 32 || puddle_devkey_from_seed(&k, reinterpret_cast<const unsigned char *>(seed.constData())) != 0)
        return QString();
    puddle_device_check(k.pub, c);
    return QString::fromLatin1(c);
}

void ApiClient::pair(const PairingLink &link, const QString &name, const QByteArray &deviceSeed, std::function<void(const Paired &)> done)
{
    puddle_devkey k;
    if (deviceSeed.size() != 32 || puddle_devkey_from_seed(&k, reinterpret_cast<const unsigned char *>(deviceSeed.constData())) != 0) {
        Paired p;
        p.error = tr("Could not make a key for this phone.");
        return done(p);
    }
    /* configure as this device for the one request, with no key for the Puddle yet: its answer brings it */
    base_ = link.base;
    puddleId_ = link.puddleId;
    seed_ = deviceSeed;
    const QByteArray pub = QByteArray(reinterpret_cast<const char *>(k.pub), 32).toHex();
    const QByteArray body = QJsonDocument(QJsonObject{ { "name", name.trimmed() }, { "key", QString::fromLatin1(pub) }, { "secret", link.secret } })
                                .toJson(QJsonDocument::Compact);
    QByteArray nonce;
    QNetworkReply *r = signedRequest("POST", QStringLiteral("/device/v1/pair"), body, "application/json", 15000, false, &nonce);
    if (!r) {
        Paired p;
        p.error = tr("Could not sign the request.");
        return done(p);
    }
    connect(r, &QNetworkReply::finished, this, [this, r, nonce, done = std::move(done), deviceSeed] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray b = r->readAll();
        r->deleteLater();
        Paired p;
        if (status == 0) {
            p.error = tr("Nothing answered at %1.").arg(base_.authority());
            return done(p);
        }
        if (status == 404) {
            p.error = tr("That link didn't work: it was used already, it is more than ten minutes old, or a newer one was made. Make a new one.");
            return done(p);
        }
        const QJsonObject o = jsonOf(b).object();
        const QJsonObject pud = o.value(QStringLiteral("puddle")).toObject();
        const QByteArray chain = pud.value(QStringLiteral("chain")).toString().toUtf8();
        puddle_chain_head h;
        char err[200];
        if (chain.isEmpty() ||
            puddle_chain_verify(chain.constData(), size_t(chain.size()), puddleId_.toLatin1().constData(), &h, err, sizeof err) != 0) {
            p.error = tr("What answered isn't the Puddle in the link.");
            return done(p);
        }
        const QByteArray key(reinterpret_cast<const char *>(h.sign_pub), 32);
        if (checkReceipt(r, status, b, nonce, key) != PUDDLE_RECEIPT_OK) {
            p.error = problem_;
            return done(p);
        }
        if (status != 202) {
            const QString code = o.value(QStringLiteral("error")).toString();
            p.error = code == QLatin1String("already_paired") ? tr("This phone is paired already.")
                    : code == QLatin1String("too_many_devices") ? tr("Your Puddle has as many devices as it takes. Remove one first.")
                    : tr("Your Puddle said no (%1).").arg(code.isEmpty() ? QString::number(status) : code);
            return done(p);
        }
        p.ok = true;
        p.deviceId = o.value(QStringLiteral("device")).toString();
        p.check = o.value(QStringLiteral("check")).toString();
        p.puddleKey = key;
        if (p.deviceId != deviceIdOf(deviceSeed) || p.check != checkOf(deviceSeed)) { /* it must be talking about this phone */
            p.ok = false;
            p.error = tr("Your Puddle's answer was about another device.");
        }
        done(p);
    });
}
