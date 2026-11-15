// The app as a phone (docs/identity.md, "Devices"): pairing with a real Puddle over TLS, the owner accepting it from the
// desktop app, then the phone's Chats using that Puddle as a thin client, signing every request and believing only
// answers the Puddle signed. And what goes wrong: a link that doesn't work, a key that isn't the Puddle's, a certificate
// that changes (nothing to approve), a phone that is removed. And calls on the phone, over a leg of its own.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "testnode.h"
#include "../src/gui/app/phone.h"
#include "federation/calls.h"
#include "service/server.h"

#include <mutex>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static QJsonObject row(JsonListModel *m, const QString &key, const QString &value)
{
    for (const QJsonObject &o : m->items())
        if (o.value(key).toString() == value)
            return o;
    return {};
}

static QByteArray signingKeyOf(const QString &dataDir)
{
    char err[300];
    puddle_identity *id = puddle_identity_open(dataDir.toUtf8().constData(), err, sizeof err);
    if (!id)
        return QByteArray();
    unsigned char pub[32];
    puddle_identity_public(id, pub, nullptr, nullptr);
    puddle_identity_close(id);
    return QByteArray(reinterpret_cast<const char *>(pub), 32);
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    CHECK(tmp.isValid());

    Node alice, bob;
    CHECK(alice.up(tmp.path(), "Alice"));
    CHECK(bob.up(tmp.path(), "Bob"));
    Chats &A = *alice.chats, &B = *bob.chats;
    CHECK(until([&] { return A.connected() && B.connected(); }));
    /* Alice and Bob are friends */
    A.request(bob.link, "hi", "main");
    CHECK(until([&] { return B.incomingRequests() == 1; }));
    B.accept(alice.uid, "main");
    auto *people_a = static_cast<JsonListModel *>(A.people());
    CHECK(until([&] { return row(people_a, "id", bob.uid).value("state").toString() == "active"; }));

    /* ---- a link that is not one, or not any more ---- */
    const QString phoneDir = tmp.path() + "/phone";
    Chats P;
    P.setInterval(100);
    Phone phone(&P, phoneDir);
    phone.setPollInterval(100);
    CHECK(phone.state() == "unpaired");
    phone.pair("https://example.org", "My phone");
    CHECK(phone.state() == "unpaired" && phone.error().contains("isn't a pairing link"));
    {
        ApiClient::PairingLink l;
        const QString id = alice.uid, secret(32, QLatin1Char('a'));
        CHECK(ApiClient::parsePairingLink("puddle-pair://203.0.113.7:8443?id=" + id + "&secret=" + secret, &l) && l.base.toString() == "https://203.0.113.7:8443");
        CHECK(ApiClient::parsePairingLink("puddle-pair://[2001:db8::1]?id=" + id + "&secret=" + secret, &l) && l.base.port() == 443);
        CHECK(!ApiClient::parsePairingLink("puddle-pair://203.0.113.7?id=short&secret=" + secret, &l));
        CHECK(!ApiClient::parsePairingLink("puddle-pair://203.0.113.7?id=" + id + "&secret=XYZ", &l));
        /* a well-formed link whose secret was never made: the Puddle says nothing, and the phone says the link didn't work */
        phone.pair(QString("puddle-pair://127.0.0.1:%1?id=%2&secret=%3").arg(alice.cfg.port).arg(alice.uid, secret), "My phone");
        CHECK(until([&] { return phone.state() == "unpaired" && phone.error().contains("didn't work"); }));
    }

    /* ---- the owner makes a link on the computer; the phone pairs with it ---- */
    QSignalSpy newDevice(&A, &Chats::newDevice);
    A.startPairing();
    CHECK(until([&] { return A.pairingLink().startsWith("puddle-pair://"); }));
    CHECK(A.pairingLink().contains(alice.uid) && A.pairingExpires() > QDateTime::currentMSecsSinceEpoch());
    phone.pair(A.pairingLink(), "  My phone ");
    CHECK(until([&] { return phone.state() == "waiting"; }));
    CHECK(phone.check().size() == 6 && phone.error().isEmpty() && phone.puddleId() == alice.uid);
    CHECK(QFile::exists(phoneDir + "/device.json"));
    CHECK((QFile(phoneDir + "/device.json").permissions() & (QFileDevice::ReadOther | QFileDevice::ReadGroup)) == 0); /* its key is in there */

    /* the computer shows it, with the same six digits, and says so */
    auto *devices = static_cast<JsonListModel *>(A.devices());
    CHECK(until([&] { return A.pendingDevices() == 1; }));
    const QJsonObject d = devices->items().value(0);
    CHECK(d.value("name").toString() == "My phone" && d.value("check").toString() == phone.check() && d.value("state").toString() == "pending");
    CHECK(until([&] { return newDevice.count() == 1; }));
    CHECK(until([&] { return A.pairingLink().isEmpty(); }));   /* used: no longer shown */

    /* waiting is all it can do */
    CHECK(phone.state() == "waiting");
    A.acceptDevice(d.value("id").toString());
    CHECK(until([&] { return phone.state() == "ready"; }));
    CHECK(until([&] { return A.pendingDevices() == 0; }));

    /* ---- the phone uses Alice's Puddle: her friends, her chats, sending as her ---- */
    auto *people_p = static_cast<JsonListModel *>(P.people());
    CHECK(until([&] { return row(people_p, "id", bob.uid).value("name").toString() == "Bob"; }));
    CHECK(P.problem().isEmpty() && P.connected());
    {
        /* and stays connected: answers over a connection already open carry no certificate in Qt, but are still believed */
        QSignalSpy flips(&P, &Chats::connectedChanged);
        until([] { return false; }, 2000);
        CHECK(flips.count() == 0 && P.connected() && P.problem().isEmpty());
        if (flips.count())
            fprintf(stderr, "flipped %d times: %s\n", int(flips.count()), qPrintable(P.problem()));
    }
    P.open(bob.uid);
    P.send("sent from my phone");
    B.open(alice.uid);
    auto *msgs_b = static_cast<JsonListModel *>(B.messages());
    CHECK(until([&] { return row(msgs_b, "body", "sent from my phone").value("outgoing").toBool() == false && msgs_b->rowCount() > 0; }));
    auto *msgs_a = static_cast<JsonListModel *>(A.messages());
    A.open(bob.uid);
    CHECK(until([&] { return !row(msgs_a, "body", "sent from my phone").isEmpty(); }));   /* it is Alice's, on the computer too */
    /* pictures come the same way (raw bytes, signed for) */
    {
        int st = -1;
        P.api()->getBytes("/health", [&](int s, const QByteArray &) { st = s; });
        CHECK(until([&] { return st != -1; }) && st == 200);
    }

    /* files too: uploaded in signed chunks, and downloaded straight from the Puddle's disk, hashed on the way and kept only
     * because the Puddle signed for exactly those bytes */
    {
        const int size = (1 << 20) + 5000;
        QByteArray bytes(size, 0);
        for (int i = 0; i < size; i++)
            bytes[i] = char(i * 7 + (i >> 11));
        QString token, fid;
        int st = -1;
        P.api()->post("/api/v1/files/uploads/create", QJsonObject{ { "size", size } }, [&](int s, const QJsonDocument &d) {
            st = s;
            token = d.object().value("upload").toString();
        });
        CHECK(until([&] { return st != -1; }) && st == 201 && token.size() == 32);
        for (int k = 0; k < 2; k++) {
            st = -1;
            P.api()->postBytes(QString("/api/v1/files/uploads/%1/chunks/%2").arg(token).arg(k), bytes.mid(k << 20, 1 << 20),
                               [&](int s, const QJsonDocument &) { st = s; });
            CHECK(until([&] { return st != -1; }) && st == 200);
        }
        st = -1;
        P.api()->post(QString("/api/v1/files/uploads/%1/finish").arg(token), QJsonObject{}, [&](int s, const QJsonDocument &d) {
            st = s;
            fid = d.object().value("id").toString();
        });
        CHECK(until([&] { return st != -1; }) && st == 201 && fid.size() == 64);
        const QString dest = tmp.path() + "/fetched.bin";
        int done = -1;
        qint64 seen = 0;
        P.api()->download(QString("/api/v1/files/%1/content").arg(fid), dest, [&](qint64 got, qint64) { seen = got; },
                          [&](bool ok) { done = ok; });
        CHECK(until([&] { return done != -1; }, 30000) && done == 1 && seen == size);
        QFile got(dest);
        CHECK(got.open(QIODevice::ReadOnly) && got.readAll() == bytes);
        CHECK(!QFile::exists(dest + ".part") && P.problem().isEmpty());
    }

#ifdef PUDDLE_HAVE_CALLS
    /* ---- calls on the phone (docs/calls.md, "On a phone"): it answers Bob, sets up a leg of its own to Alice's Puddle,
     * and the Puddle passes the Opus frames between that leg and Bob ---- */
    {
        puddle_calls *bobEngine = puddle_server_calls(bob.srv), *aliceEngine = puddle_server_calls(alice.srv);
        CHECK(bobEngine && aliceEngine);
        struct Heard {
            std::mutex mu;
            QList<QByteArray> frames;
            int count() { std::lock_guard<std::mutex> g(mu); return int(frames.size()); }
            void clear() { std::lock_guard<std::mutex> g(mu); frames.clear(); }
        } atBob, atPhone;
        puddle_calls_on_audio(bobEngine, [](void *u, const char *, const unsigned char *f, size_t n, uint32_t) {
            auto *h = static_cast<Heard *>(u);
            std::lock_guard<std::mutex> g(h->mu);
            h->frames.append(QByteArray(reinterpret_cast<const char *>(f), qsizetype(n)));
        }, &atBob);
        Calls &pc = *P.callModel(), &ac = *A.callModel(), &bc = *B.callModel();
        CallLeg *leg = pc.leg();
        CHECK(leg && !ac.leg() && !bc.leg());
        leg->setOnFrame([&](uint32_t, const QByteArray &f) {
            std::lock_guard<std::mutex> g(atPhone.mu);
            atPhone.frames.append(f);
        });
        auto frame = [](char tag, int i) { return QByteArray(1, tag) + QByteArray::number(i) + QByteArray(40, char(i)); };

        /* Bob rings: the phone and the computer both ring; the phone answers */
        QSignalSpy rangPhone(&pc, &Calls::ringing), rangComputer(&ac, &Calls::ringing);
        bc.call(alice.uid);
        CHECK(until([&] { return pc.state() == "ringing" && ac.state() == "ringing"; }));
        CHECK(rangPhone.count() == 1 && rangComputer.count() == 1 && pc.elsewhere().isEmpty());
        const QString id = pc.callId();
        pc.accept();
        CHECK(until([&] { return pc.state() == "active" && pc.here() && leg->connected(); }));
        CHECK(leg->callId() == id);
        /* the computer sees where it went, and has no sound of its own */
        CHECK(until([&] { return ac.state() == "active" && !ac.here() && ac.elsewhere() == "on My phone"; }));
        {
            const unsigned char f[3] = { 1, 2, 3 };
            CHECK(puddle_call_send_audio(aliceEngine, id.toUtf8().constData(), f, sizeof f) == -1);
        }
        int st = -1;
        QJsonObject item;
        CHECK(until([&] {
            st = -1;
            A.api()->get("/api/v1/calls/" + id, [&](int s, const QJsonDocument &d) { st = s, item = d.object(); });
            until([&] { return st != -1; });
            return item.value("media").toBool();
        }));
        CHECK(item.value("device").toObject().value("name").toString() == "My phone");
        /* frames both ways: Bob's reach the phone, the phone's reach Bob */
        for (int i = 0; i < 30; i++) {
            const QByteArray f = frame('b', i);
            puddle_call_send_audio(bobEngine, id.toUtf8().constData(), reinterpret_cast<const unsigned char *>(f.constData()), size_t(f.size()));
            CHECK(leg->send(frame('p', i)));
            until([] { return false; }, 20);
        }
        CHECK(until([&] { return atPhone.count() >= 25 && atBob.count() >= 25; }));
        {
            std::lock_guard<std::mutex> g(atPhone.mu);
            CHECK(atPhone.frames.value(0).startsWith('b') && atPhone.frames.contains(frame('b', 29)));
        }
        {
            std::lock_guard<std::mutex> g(atBob.mu);
            CHECK(atBob.frames.value(0).startsWith('p') && atBob.frames.contains(frame('p', 29)));
        }
        /* only a phone sets up a leg */
        st = -1;
        A.api()->post("/api/v1/calls/" + id + "/media", QJsonObject{ { "sdp", "v=0" } }, [&](int s, const QJsonDocument &d) {
            st = s;
            item = d.object();
        });
        CHECK(until([&] { return st != -1; }) && st == 400 && item.value("error").toString() == "not_a_phone");
        /* the phone hangs up: Bob hears so, and the leg is gone */
        pc.hangUp();
        CHECK(until([&] { return pc.state().isEmpty() && bc.state().isEmpty() && ac.state().isEmpty(); }));
        CHECK(until([&] { return leg->callId().isEmpty() && !leg->connected(); }));

        /* the phone rings Bob, who answers on his computer */
        atPhone.clear(), atBob.clear();
        pc.call(bob.uid);
        CHECK(until([&] { return bc.state() == "ringing" && pc.state() == "calling" && pc.here(); }));
        CHECK(until([&] { return ac.state() == "calling" && !ac.here() && ac.elsewhere() == "on My phone"; }));
        bc.accept();
        CHECK(until([&] { return pc.state() == "active" && leg->connected() && bc.state() == "active"; }));
        const QString id2 = pc.callId();
        CHECK(id2 != id && leg->callId() == id2);
        for (int i = 0; i < 20; i++) {
            const QByteArray f = frame('b', i);
            puddle_call_send_audio(bobEngine, id2.toUtf8().constData(), reinterpret_cast<const unsigned char *>(f.constData()), size_t(f.size()));
            leg->send(frame('p', i));
            until([] { return false; }, 20);
        }
        CHECK(until([&] { return atPhone.count() >= 15 && atBob.count() >= 15; }));
        /* Bob hangs up */
        bc.hangUp();
        CHECK(until([&] { return pc.state().isEmpty() && leg->callId().isEmpty(); }));

        /* the phone goes (the app is closed, the network gone): it stops asking, so its Puddle ends the call, and Bob's ends */
        pc.call(bob.uid);
        CHECK(until([&] { return bc.state() == "ringing"; }));
        bc.accept();
        CHECK(until([&] { return pc.state() == "active" && leg->connected(); }));
        P.pause();
        CHECK(until([&] { return bc.state().isEmpty() && ac.state().isEmpty(); }, 20000));
        P.resume();
        CHECK(until([&] { return pc.state().isEmpty(); }));
        puddle_calls_on_audio(bobEngine, nullptr, nullptr);
        leg->setOnFrame(nullptr);
    }
#endif

    /* ...but not managing phones: the Puddle answers that as it would a stranger, and the phone says what it saw */
    {
        int st = -1;
        P.api()->get("/api/v1/devices", [&](int s, const QJsonDocument &) { st = s; });
        CHECK(until([&] { return st != -1; }) && st == 0 && P.problem().contains("didn't recognise"));
    }

    /* ---- the phone is closed and opened again: it remembers ---- */
    {
        Chats P2;
        P2.setInterval(100);
        Phone again(&P2, phoneDir);
        CHECK(again.state() == "ready");
        CHECK(until([&] { return row(static_cast<JsonListModel *>(P2.people()), "id", bob.uid).value("name").toString() == "Bob"; }));
    }

    /* ---- only Alice's Puddle will do. Her id is what the phone goes by: holding some other key for her (as after she rolled
     * hers), it takes the one her chain proves, and believes nothing else ---- */
    {
        ApiClient c;
        QFile df(phoneDir + "/device.json");
        CHECK(df.open(QIODevice::ReadOnly));
        const QByteArray seed = QByteArray::fromHex(QJsonDocument::fromJson(df.readAll()).object().value("seed").toString().toLatin1());
        CHECK(c.configureDevice(QUrl(QString("https://127.0.0.1:%1").arg(alice.cfg.port)), alice.uid, signingKeyOf(bob.cfg.data_dir), seed));
        QSignalSpy rolled(&c, &ApiClient::puddleKeyChanged);
        int st = -1;
        c.get("/health", [&](int s, const QJsonDocument &) { st = s; });
        CHECK(until([&] { return st != -1; }) && st == 200 && rolled.count() == 1 && c.puddleKey() == signingKeyOf(alice.cfg.data_dir));
        /* nor Bob's, at Bob's address, with Alice's key: what answered isn't her Puddle */
        ApiClient c2;
        CHECK(c2.configureDevice(QUrl(QString("https://127.0.0.1:%1").arg(bob.cfg.port)), alice.uid, signingKeyOf(alice.cfg.data_dir), seed));
        st = -1;
        c2.get("/health", [&](int s, const QJsonDocument &) { st = s; });
        CHECK(until([&] { return st != -1; }) && st == 0 && c2.lastProblem().contains("didn't recognise"));
    }

    /* ---- Alice's Puddle gets a new certificate: the phone carries on, nobody asked ---- */
    {
        alice.stop();
        QFile::remove(alice.dir + "/cert.pem");
        QFile::remove(alice.dir + "/key.pem");
        CHECK(alice.start());
        int st = -1;
        P.api()->get("/api/v1/identity", [&](int s, const QJsonDocument &) { st = s; });
        CHECK(until([&] { return st != -1; }) && st == 200);
        CHECK(P.problem().isEmpty());
        /* the computer's own window reads the new certificate when it is started again, as it always has */
        CHECK(A.attach(QUrl(QString("https://127.0.0.1:%1").arg(alice.cfg.port)), alice.dir + "/data/state/owner-token", alice.dir + "/cert.pem"));
    }

    /* ---- removed on the computer: the phone is shut out, and says why ---- */
    CHECK(until([&] { return devices->rowCount() == 1; }));
    QSignalSpy failedA(&A, &Chats::failed);
    A.revokeDevice(d.value("id").toString());
    if (!until([&] { return devices->rowCount() == 0; }, 3000))
        fprintf(stderr, "revoke: failed=%d rows=%d\n", int(failedA.count()), devices->rowCount());
    CHECK(until([&] { return devices->rowCount() == 0; }));
    {
        int st = -1;
        P.api()->get("/api/v1/identity", [&](int s, const QJsonDocument &) { st = s; });
        CHECK(until([&] { return st != -1; }) && st == 0);
        CHECK(P.problem().contains("didn't recognise") || P.problem().contains("no longer"));
    }
    /* and it can start over */
    phone.forget();
    CHECK(phone.state() == "unpaired" && !QFile::exists(phoneDir + "/device.json"));

    return failures ? 1 : 0;
}
