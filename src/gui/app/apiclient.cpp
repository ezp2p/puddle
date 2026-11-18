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
#include "base/encoding.h"
#include "base/seal.h"
#include "http/reqsig.h"


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
    if (!base.isValid() || puddleId.size() != PUDDLE_USER_ID_LEN || puddleKey.size() != 64 || deviceSeed.size() != 32 ||
        puddle_devkey_from_seed(&k, reinterpret_cast<const unsigned char *>(deviceSeed.constData())) != 0)
        return false;
    base_ = base;
    token_.clear();
    device_ = true;
    puddleId_ = puddleId;
    puddleKey_ = puddleKey;
    seed_ = deviceSeed;
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
//
// Every request is sealed to the Puddle's sealing key and signed with the phone's own; the answer comes back sealed, and
// counts only with the Puddle's receipt (the protocol's spec/09). TLS only carries it: no certificate is checked, so the
// Puddle can sit behind a reverse proxy, on any certificate, changed whenever.

void ApiClient::setProblem(const QString &why)
{
    problem_ = why;
    if (!why.isEmpty())
        emit problem(why);
}

QNetworkReply *ApiClient::signedRequest(const QByteArray &method, const QString &path, const QByteArray &body, int timeoutMs,
                                        const QByteArray &sealKey, QByteArray *nonce, QByteArray *answerKey)
{
    const QUrl url(base_.toString() + path);
    const QByteArray target = url.toEncoded(QUrl::RemoveScheme | QUrl::RemoveAuthority); // path and query, exactly as sent
    puddle_devkey k;
    puddle_sealed_request sealed;
    if (sealKey.size() != 32 || puddle_devkey_from_seed(&k, reinterpret_cast<const unsigned char *>(seed_.constData())) != 0 ||
        puddle_seal_request_as(k.id, k.pub, puddle_devkey_sign, &k, method.constData(), target.constData(), body.constData(),
                               size_t(body.size()), puddleId_.toLatin1().constData(),
                               reinterpret_cast<const unsigned char *>(sealKey.constData()), 0, &sealed) != 0)
        return nullptr;
    *nonce = QByteArray(sealed.nonce);
    *answerKey = QByteArray(reinterpret_cast<const char *>(sealed.answer_key), 32);
    const QByteArray sealedBody = sealed.body ? QByteArray(reinterpret_cast<const char *>(sealed.body), qsizetype(sealed.body_len)) : QByteArray();
    QNetworkRequest req(url);
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setPeerVerifyMode(QSslSocket::VerifyNone); // TLS only carries it: the seal keeps it private, the receipt says who answered
    req.setSslConfiguration(ssl);
    req.setRawHeader("X-Puddle-Signature", sealed.signature);
    req.setRawHeader("X-Puddle-Seal", sealed.seal);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(timeoutMs);
    puddle_sealed_request_free(&sealed);
    QNetworkAccessManager &via = *manager(true);
    QNetworkReply *r;
    if (method == "POST") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray(PUDDLE_SEALED_TYPE));
        r = via.post(req, sealedBody);
    } else {
        r = via.get(req);
    }
    allowHostMismatch(r);
    return r;
}

bool ApiClient::openAnswer(QNetworkReply *r, int status, const QByteArray &raw, const QByteArray &answerKey, QByteArray *plain)
{
    const QByteArray type = r->header(QNetworkRequest::ContentTypeHeader).toByteArray().split(';').value(0).trimmed().toLower();
    if (type != PUDDLE_SEALED_TYPE) { /* a refusal that couldn't be sealed (or a 204, which has nothing to seal) comes plain */
        *plain = raw;
        return status == 204 || status >= 400;
    }
    QByteArray out(qsizetype(puddle_seal_plain_len(size_t(raw.size()))) + 1, '\0');
    size_t got = 0;
    if (puddle_open_answer(reinterpret_cast<const unsigned char *>(answerKey.constData()), status,
                           reinterpret_cast<const unsigned char *>(raw.constData()), size_t(raw.size()),
                           reinterpret_cast<unsigned char *>(out.data()), &got) != 0)
        return false;
    out.truncate(qsizetype(got));
    *plain = out;
    return true;
}

int ApiClient::checkReceipt(QNetworkReply *r, int status, const QByteArray &body, const QByteArray &nonce, const QByteArray &key,
                            const QByteArray &bodyHash)
{
    const QByteArray receipt = r->rawHeader("X-Puddle-Receipt");
    const QByteArray hash = bodyHash.isEmpty() ? QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex() : bodyHash;
    const int rc = puddle_receipt_verify_hash(receipt.isEmpty() ? nullptr : receipt.constData(), puddleId_.toLatin1().constData(),
                                              reinterpret_cast<const unsigned char *>(key.constData()), nonce.constData(), status,
                                              hash.constData());
    const QString where = base_.authority();
    switch (rc) {
    case PUDDLE_RECEIPT_OK: problem_.clear(); break;
    case PUDDLE_RECEIPT_MISSING:
        problem_ = status == 404 ? tr("Your Puddle at %1 didn't recognise this phone: it isn't your Puddle, or this phone was removed from it.").arg(where)
                                 : tr("Something at %1 answered, but it isn't your Puddle.").arg(where);
        break;
    case PUDDLE_RECEIPT_WRONG_KEY: problem_ = tr("What answered at %1 couldn't prove it is your Puddle.").arg(where); break;
    default: problem_ = tr("What answered at %1 couldn't prove it is your Puddle (its signature doesn't check out).").arg(where); break;
    }
    return rc;
}

/* The Puddle has rolled its keys if an answer signed by a key we don't hold brings a chain that is our Puddle's (by its id)
 * and whose key signed it (its `stale_key` refusal, and /device/v1/status, carry the chain). Then that key is kept. */
bool ApiClient::learnRoll(QNetworkReply *r, int status, const QByteArray &raw, const QByteArray &plain, const QByteArray &nonce)
{
    const QByteArray chain = jsonOf(plain).object().value(QStringLiteral("chain")).toString().toUtf8();
    puddle_chain_head h;
    char err[200];
    if (chain.isEmpty() || puddle_chain_verify(chain.constData(), size_t(chain.size()), puddleId_.toLatin1().constData(), &h, err, sizeof err) != 0)
        return false;
    const QByteArray sign(reinterpret_cast<const char *>(h.sign_pub), 32);
    if (checkReceipt(r, status, raw, nonce, sign) != PUDDLE_RECEIPT_OK)
        return false;
    puddleKey_ = sign + QByteArray(reinterpret_cast<const char *>(h.enc_pub), 32);
    emit puddleKeyChanged(puddleKey_);
    return true;
}

void ApiClient::deviceSend(Pending p)
{
    if (!p.dest.isEmpty())
        return deviceDownload(std::move(p));
    QByteArray nonce, answerKey;
    QNetworkReply *r = signedRequest(p.method, p.path, p.body, p.timeoutMs, puddleKey_.mid(32), &nonce, &answerKey);
    if (!r)
        return p.done(0, QByteArray());
    connect(r, &QNetworkReply::finished, this, [this, r, nonce, answerKey, p = std::move(p)]() mutable {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = r->readAll();
        r->deleteLater();
        if (status == 0 && p.tries > 0) {
            /* a kept connection to a Puddle that has restarted since (with a new certificate, say): once more, afresh */
            renew(open_);
            p.tries--;
            return deviceSend(std::move(p));
        }
        if (status == 0) {
            problem_ = tr("Your Puddle can't be reached at %1.").arg(base_.authority());
            return p.done(0, QByteArray());
        }
        const int rc = checkReceipt(r, status, raw, nonce, puddleKey_.left(32));
        QByteArray plain;
        const bool opened = openAnswer(r, status, raw, answerKey, &plain);
        if (rc == PUDDLE_RECEIPT_OK && opened && !(status == 409 && jsonOf(plain).object().value(QStringLiteral("error")).toString() == QLatin1String("stale_key")))
            return p.done(status, plain);
        if (opened && p.tries > 0 && learnRoll(r, status, raw, plain, nonce)) { /* it rolled its keys: again, with the new ones */
            p.tries--;
            return deviceSend(std::move(p));
        }
        if (rc == PUDDLE_RECEIPT_OK && !opened)
            problem_ = tr("Your Puddle's answer was sealed, but not for this phone.");
        setProblem(problem_);
        p.done(0, QByteArray());
    });
}

/* A phone's download: opened record by record as it comes (spec/09, "Sealing an answer") and written to "<dest>.part", the
 * sealed bytes hashed on the way, then kept only if the Puddle signed for exactly those bytes. A record is opened once the
 * next has begun, since only the end of the answer says which one is the last. */
void ApiClient::deviceDownload(Pending p)
{
    auto *file = new QFile(p.dest + ".part");
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        return p.fetched(false);
    }
    QByteArray nonce, answerKey;
    QNetworkReply *r = signedRequest("GET", p.path, QByteArray(), p.timeoutMs, puddleKey_.mid(32), &nonce, &answerKey);
    if (!r) {
        file->remove();
        delete file;
        return p.fetched(false);
    }
    struct Opening {
        QCryptographicHash hash{ QCryptographicHash::Sha256 };
        QByteArray pending;
        quint64 index = 0;
        bool bad = false;
    };
    auto st = std::make_shared<Opening>();
    const qsizetype Full = PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN;
    auto openRecord = [st, file, answerKey](const QByteArray &rec, int status, bool last) {
        QByteArray out(rec.size(), '\0');
        if (rec.size() < PUDDLE_SEAL_TAG_LEN ||
            puddle_open_record(reinterpret_cast<const unsigned char *>(answerKey.constData()), st->index, status, last ? 1 : 0,
                               reinterpret_cast<const unsigned char *>(rec.constData()), size_t(rec.size()),
                               reinterpret_cast<unsigned char *>(out.data())) != 0) {
            st->bad = true;
            return;
        }
        st->index++;
        file->write(out.constData(), rec.size() - PUDDLE_SEAL_TAG_LEN);
    };
    connect(r, &QNetworkReply::readyRead, this, [r, st, openRecord, Full] {
        const QByteArray got = r->readAll();
        st->hash.addData(got);
        st->pending += got;
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        while (!st->bad && st->pending.size() > Full) { /* more follows this one, so it isn't the last */
            openRecord(st->pending.left(Full), status, false);
            st->pending.remove(0, Full);
        }
    });
    auto progress = p.progress;
    connect(r, &QNetworkReply::downloadProgress, this, [progress](qint64 got, qint64 total) { if (progress) progress(got, total); });
    connect(r, &QNetworkReply::finished, this, [this, r, file, st, openRecord, nonce, Full, p = std::move(p)]() mutable {
        const QByteArray rest = r->readAll();
        st->hash.addData(rest);
        st->pending += rest;
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray type = r->header(QNetworkRequest::ContentTypeHeader).toByteArray().split(';').value(0).trimmed().toLower();
        bool came = r->error() == QNetworkReply::NoError && status == 200 && type == PUDDLE_SEALED_TYPE;
        while (came && !st->bad && st->pending.size() > Full) {
            openRecord(st->pending.left(Full), status, false);
            st->pending.remove(0, Full);
        }
        if (came && !st->bad)
            openRecord(st->pending, status, true);
        came = came && !st->bad && file->flush();
        file->close();
        r->deleteLater();
        bool ok = false;
        if (came) {
            ok = checkReceipt(r, status, QByteArray(), nonce, puddleKey_.left(32), st->hash.result().toHex()) == PUDDLE_RECEIPT_OK;
            if (!ok)
                setProblem(problem_);
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

// ---- pairing ----

bool ApiClient::parsePairingLink(const QString &link, PairingLink *out)
{
    const QUrl u(link.trimmed());
    if (!u.isValid() || u.scheme() != QLatin1String("puddle-pair") || u.host().isEmpty())
        return false;
    const QUrlQuery q(u);
    const QString id = q.queryItemValue(QStringLiteral("id")), secret = q.queryItemValue(QStringLiteral("secret")),
                  key = q.queryItemValue(QStringLiteral("k"));
    unsigned char sealKey[32];
    if (puddle_base32_decode(key.toLatin1().constData(), sealKey, 32) != 0)
        return false;
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
    out->sealKey = QByteArray(reinterpret_cast<const char *>(sealKey), 32);
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
    QByteArray nonce, answerKey;
    QNetworkReply *r = signedRequest("POST", QStringLiteral("/device/v1/pair"), body, 15000, link.sealKey, &nonce, &answerKey);
    if (!r) {
        Paired p;
        p.error = tr("Could not sign the request.");
        return done(p);
    }
    const QByteArray linkKey = link.sealKey;
    connect(r, &QNetworkReply::finished, this, [this, r, nonce, answerKey, linkKey, done = std::move(done), deviceSeed] {
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = r->readAll();
        r->deleteLater();
        QByteArray b;
        if (status != 0 && !openAnswer(r, status, raw, answerKey, &b)) {
            Paired p;
            p.error = tr("What answered isn't the Puddle in the link.");
            return done(p);
        }
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
        const QByteArray key(reinterpret_cast<const char *>(h.sign_pub), 32), seal(reinterpret_cast<const char *>(h.enc_pub), 32);
        if (seal != linkKey) { /* the link named whose Puddle it is: one with another key is someone else's */
            p.error = tr("What answered isn't the Puddle in the link.");
            return done(p);
        }
        if (checkReceipt(r, status, raw, nonce, key) != PUDDLE_RECEIPT_OK) {
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
        p.puddleKey = key + seal;
        if (p.deviceId != deviceIdOf(deviceSeed) || p.check != checkOf(deviceSeed)) { /* it must be talking about this phone */
            p.ok = false;
            p.error = tr("Your Puddle's answer was about another device.");
        }
        done(p);
    });
}
