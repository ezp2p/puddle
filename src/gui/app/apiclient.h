#pragma once

#include <QJsonDocument>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSslCertificate>
#include <QUrl>
#include <functional>

// Talks to the owner API of a Puddle over HTTPS. Only GET and POST are used. Two ways in:
//
// - the desktop app, on the Puddle's own machine (configure): the Puddle's certificate file is the only one trusted
//   (pinned), and the owner token goes with every request.
// - a phone (configureDevice; docs/identity.md, "Devices"): no token and no certificate. Every request is sealed to the
//   Puddle's sealing key and signed with the phone's own key, and an answer counts only if it carries the Puddle's receipt,
//   signed by the key the phone holds for it, and opens with the request's key (the protocol's spec/09). TLS only carries
//   it. After a key roll an answer brings the Puddle's chain, which proves the new keys against the Puddle's id.
class ApiClient : public QObject {
    Q_OBJECT
public:
    // status 0 = the Puddle could not be reached, or (a phone) what answered could not prove it is the Puddle;
    // `json` is null when the body was not JSON (e.g. 204).
    using Reply = std::function<void(int status, const QJsonDocument &json)>;
    using RawReply = std::function<void(int status, const QByteArray &body)>;

    explicit ApiClient(QObject *parent = nullptr);

    // Qt's network manager keeps about 8 KB for every TLS connection it opens until it is destroyed (seen with Qt 6.12),
    // and a Puddle closes the connection after each answer: polling made the window grow by gigabytes over a few days.
    // So each manager is used for this many requests and then replaced; an old one goes once its last reply has.
    static constexpr int UsesPerManager = 256;

    // The desktop app. `certPem`: the Puddle's own certificate file. `token`: the owner token.
    bool configure(const QUrl &base, const QByteArray &token, const QString &certPemPath);
    // A phone. `puddleKey`: the Puddle's current signing key then its sealing key (32 + 32 bytes); `deviceSeed`: the phone's
    // own key (32 bytes).
    bool configureDevice(const QUrl &base, const QString &puddleId, const QByteArray &puddleKey, const QByteArray &deviceSeed);
    bool ready() const { return ready_; }
    bool isDevice() const { return device_; }
    QByteArray puddleKey() const { return puddleKey_; }
    QString lastProblem() const { return problem_; } // a phone: why the last answer was not believed; "" if it was

    void get(const QString &path, Reply done);
    void post(const QString &path, const QJsonObject &body, Reply done);
    // POST `bytes` as they are (a chunk of a file being uploaded).
    void postBytes(const QString &path, const QByteArray &bytes, Reply done);

    // GET `path` into the file `dest`, written as it arrives (a file of any size): `progress(received, total)` on the way,
    // `done(ok)` at the end. Nothing is left at `dest` unless the whole of it came (and, on a phone, the Puddle signed for it).
    void download(const QString &path, const QString &dest, std::function<void(qint64, qint64)> progress, std::function<void(bool)> done);

    // A picture or other file the Puddle holds, by its path ("/api/v1/media?hash=..."), as raw bytes.
    void getBytes(const QString &path, std::function<void(int status, const QByteArray &bytes)> done);

    // ---- pairing a phone (docs/identity.md, "Devices") ----
    struct PairingLink {
        QUrl base;        // https://<address>
        QString puddleId;
        QString secret;
        QByteArray sealKey; // the Puddle's sealing key, from `k`: the pairing request is sealed to it
    };
    // `puddle-pair://<address>?id=<52>&k=<52>&secret=<32 hex>`; false if it is not one.
    static bool parsePairingLink(const QString &link, PairingLink *out);
    struct Paired {
        bool ok = false;
        QString error;       // in words, when not ok
        QString deviceId, check;
        QByteArray puddleKey; // signing then sealing key, proven by the chain against the id and key in the link, and by the receipt
    };
    // Ask the Puddle in `link` to pair this phone (`deviceSeed`, a fresh 32-byte key) under `name`.
    void pair(const PairingLink &link, const QString &name, const QByteArray &deviceSeed, std::function<void(const Paired &)> done);

    // Make a phone's key: 32 random bytes to keep.
    static QByteArray newDeviceSeed();
    // The device id and check code for a seed.
    static QString deviceIdOf(const QByteArray &seed);
    static QString checkOf(const QByteArray &seed);

signals:
    void puddleKeyChanged(const QByteArray &key); // a phone: the Puddle rolled its keys, and proved the new one; keep it
    void problem(const QString &why);             // a phone: something answered that could not prove it is the Puddle

private:
    struct Pending {
        QByteArray method, body, contentType;
        QString path;
        RawReply done;
        int tries = 1;
        int timeoutMs = 15000;
        // a download: written to `dest` as it comes, and only kept if the receipt (over the whole of it) is good
        QString dest;
        std::function<void(qint64, qint64)> progress;
        std::function<void(bool)> fetched;
    };
    QNetworkRequest request(const QString &path) const;
    void watch(QNetworkReply *r, Reply done);
    void issue(const QByteArray &method, const QString &path, const QByteArray &body, const QByteArray &contentType, int timeoutMs, RawReply done);
    QNetworkAccessManager *manager(bool open);
    void renew(QNetworkAccessManager *&m);   // a new one now: also how kept connections (and their trust) are dropped

    // phone
    void deviceSend(Pending p);
    void deviceDownload(Pending p);
    QNetworkReply *signedRequest(const QByteArray &method, const QString &path, const QByteArray &body, int timeoutMs, const QByteArray &sealKey,
                                 QByteArray *nonce, QByteArray *answerKey);
    // the answer's body, opened: false if it was sealed but not for us, or plain when it had no reason to be
    static bool openAnswer(QNetworkReply *r, int status, const QByteArray &raw, const QByteArray &answerKey, QByteArray *plain);
    bool learnRoll(QNetworkReply *r, int status, const QByteArray &raw, const QByteArray &plain, const QByteArray &nonce);
    // 0 believed; otherwise a puddle_receipt_check, with the reason in problem_. `bodyHash`: hex SHA-256, when the body
    // was not kept (a download); else the hash of `body` is used.
    int checkReceipt(QNetworkReply *r, int status, const QByteArray &body, const QByteArray &nonce, const QByteArray &key,
                     const QByteArray &bodyHash = QByteArray());
    void setProblem(const QString &why);

    QNetworkAccessManager *net_ = nullptr;
    QNetworkAccessManager *open_ = nullptr;  // a phone's requests that trust whatever answers (the handshake, pairing): their receipt decides
    int netUses_ = 0, openUses_ = 0;
    QUrl base_;
    QByteArray token_;
    QSslConfiguration ssl_;
    bool ready_ = false;

    bool device_ = false;
    QString puddleId_;
    QByteArray puddleKey_, seed_;
    QString problem_;
};
